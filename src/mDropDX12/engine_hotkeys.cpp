// engine_hotkeys.cpp — Configurable hotkey load/save/register/dispatch
//
// Part of the MDropDX12 configurable hotkeys system.
// Supports local (render-window-focus) and global (system-wide) bindings.
// ~84 reassignable bindings; F1/F2/Ctrl+F2/Escape remain hardcoded.

#include "engine.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "wasabi.h"
#include <TlHelp32.h>
#include <thread>

namespace mdrop {

extern Engine g_engine;
extern int ToggleFPSNumPressed;
extern int HardcutMode;
extern float timetick;
extern float timetick2;
extern float TimeToAutoLockPreset;
extern int beatcount;
extern bool TranspaMode;
extern bool AutoLockedPreset;

void ToggleTransparency(HWND hwnd);
void ToggleWindowOpacity(HWND hwnd, bool bDown);

// Helper: map a VK_OEM code for special characters on US QWERTY.
// We use fixed VK_OEM codes so defaults are keyboard-layout-independent.
// VK_OEM_4 = [  VK_OEM_6 = ]  VK_OEM_COMMA = ,  VK_OEM_PERIOD = .
// VK_OEM_3 = `  VK_OEM_MINUS = -  VK_OEM_PLUS = =+
// VK_OEM_1 = ;  VK_OEM_2 = /  VK_OEM_5 = backslash  VK_OEM_7 = '
// For shifted variants ({, }, <, >, ~, !, @) we use the base VK + MOD_SHIFT.

#define HK_DEF(idx, _id, _mod, _vk, _scope, _cat, _action, _ini) \
    m_hotkeys[idx] = { _id, _mod, _vk, _scope, _cat, \
                        _action, _ini, _mod, _vk, _scope, \
                        0, 0, 0, 0 }

// Variant with both local and global default bindings
#define HK_DEF2(idx, _id, _mod, _vk, _gmod, _gvk, _cat, _action, _ini) \
    m_hotkeys[idx] = { _id, _mod, _vk, HKSCOPE_LOCAL, _cat, \
                        _action, _ini, _mod, _vk, HKSCOPE_LOCAL, \
                        _gmod, _gvk, _gmod, _gvk }

void Engine::ResetHotkeyDefaults()
{
    memset(m_hotkeys, 0, sizeof(m_hotkeys));
    int i = 0;

    // ── Navigation ──
    HK_DEF(i++, HK_NEXT_PRESET,       0,                     VK_SPACE,     HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Next Preset",           L"NextPreset");
    HK_DEF(i++, HK_PREV_PRESET,       0,                     VK_BACK,      HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Previous Preset",       L"PrevPreset");
    HK_DEF(i++, HK_HARD_CUT,          0,                     'H',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Hard Cut",              L"HardCut");
    HK_DEF(i++, HK_RANDOM_MASHUP,     0,                     'A',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Random Mashup",         L"RandomMashup");
    HK_DEF(i++, HK_LOCK_PRESET,       0,                     VK_OEM_3,     HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Lock/Unlock Preset",    L"LockPreset");
    HK_DEF(i++, HK_TOGGLE_RANDOM,     0,                     'R',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Random/Sequential",     L"ToggleRandom");
    HK_DEF(i++, HK_OPEN_PRESET_LIST,  0,                     'L',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Preset Browser",        L"OpenPresetList");
    HK_DEF(i++, HK_SAVE_PRESET,       0,                     'S',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Save Preset As...",     L"SavePreset");
    HK_DEF(i++, HK_OPEN_MENU,         0,                     'M',          HKSCOPE_LOCAL, HKCAT_NAVIGATION, L"Toggle Menu",           L"OpenMenu");

    // ── Visual ──
    HK_DEF(i++, HK_OPACITY_UP,        MOD_SHIFT,             VK_UP,        HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity Up",            L"OpacityUp");
    HK_DEF(i++, HK_OPACITY_DOWN,      MOD_SHIFT,             VK_DOWN,      HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity Down",          L"OpacityDown");
    HK_DEF(i++, HK_OPACITY_25,        0,                     0,            HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity 25%",           L"Opacity25");
    HK_DEF(i++, HK_OPACITY_50,        0,                     0,            HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity 50%",           L"Opacity50");
    HK_DEF(i++, HK_OPACITY_75,        0,                     0,            HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity 75%",           L"Opacity75");
    HK_DEF(i++, HK_OPACITY_100,       0,                     0,            HKSCOPE_LOCAL, HKCAT_VISUAL, L"Opacity 100%",          L"Opacity100");
    HK_DEF(i++, HK_WAVE_MODE_NEXT,    0,                     'W',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Mode +",           L"WaveModeNext");
    HK_DEF(i++, HK_WAVE_MODE_PREV,    MOD_SHIFT,             'W',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Mode -",           L"WaveModePrev");
    HK_DEF(i++, HK_WAVE_ALPHA_DOWN,   0,                     'E',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Alpha -",          L"WaveAlphaDown");
    HK_DEF(i++, HK_WAVE_ALPHA_UP,     MOD_SHIFT,             'E',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Alpha +",          L"WaveAlphaUp");
    HK_DEF(i++, HK_WAVE_SCALE_DOWN,   0,                     'J',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Scale -",          L"WaveScaleDown");
    HK_DEF(i++, HK_WAVE_SCALE_UP,     MOD_SHIFT,             'J',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Wave Scale +",          L"WaveScaleUp");
    HK_DEF(i++, HK_ZOOM_IN,           0,                     'I',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Zoom In",               L"ZoomIn");
    HK_DEF(i++, HK_ZOOM_OUT,          MOD_SHIFT,             'I',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Zoom Out",              L"ZoomOut");
    HK_DEF(i++, HK_WARP_AMOUNT_DOWN,  0,                     'O',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Warp Amount -",         L"WarpAmtDown");
    HK_DEF(i++, HK_WARP_AMOUNT_UP,    MOD_SHIFT,             'O',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Warp Amount +",         L"WarpAmtUp");
    HK_DEF(i++, HK_WARP_SCALE_DOWN,   0,                     'U',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Warp Scale -",          L"WarpScaleDown");
    HK_DEF(i++, HK_WARP_SCALE_UP,     MOD_SHIFT,             'U',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Warp Scale +",          L"WarpScaleUp");
    HK_DEF(i++, HK_ECHO_ALPHA_DOWN,   0,                     'P',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Echo Alpha -",          L"EchoAlphaDown");
    HK_DEF(i++, HK_ECHO_ALPHA_UP,     MOD_SHIFT,             'P',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Echo Alpha +",          L"EchoAlphaUp");
    HK_DEF(i++, HK_ECHO_ZOOM_DOWN,    0,                     'Q',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Echo Zoom -",           L"EchoZoomDown");
    HK_DEF(i++, HK_ECHO_ZOOM_UP,      MOD_SHIFT,             'Q',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Echo Zoom +",           L"EchoZoomUp");
    HK_DEF(i++, HK_ECHO_ORIENT,       0,                     'F',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Echo Orientation",      L"EchoOrient");
    HK_DEF(i++, HK_GAMMA_DOWN,        0,                     'G',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Gamma -",               L"GammaDown");
    HK_DEF(i++, HK_GAMMA_UP,          MOD_SHIFT,             'G',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Gamma +",               L"GammaUp");
    HK_DEF(i++, HK_PUSH_X_NEG,        0,                     VK_OEM_4,     HKSCOPE_LOCAL, HKCAT_VISUAL, L"Push X -",              L"PushXNeg");
    HK_DEF(i++, HK_PUSH_X_POS,        0,                     VK_OEM_6,     HKSCOPE_LOCAL, HKCAT_VISUAL, L"Push X +",              L"PushXPos");
    HK_DEF(i++, HK_PUSH_Y_NEG,        MOD_SHIFT,             VK_OEM_4,     HKSCOPE_LOCAL, HKCAT_VISUAL, L"Push Y -",              L"PushYNeg");
    HK_DEF(i++, HK_PUSH_Y_POS,        MOD_SHIFT,             VK_OEM_6,     HKSCOPE_LOCAL, HKCAT_VISUAL, L"Push Y +",              L"PushYPos");
    HK_DEF(i++, HK_ROTATE_LEFT,       MOD_SHIFT,             VK_OEM_COMMA, HKSCOPE_LOCAL, HKCAT_VISUAL, L"Rotate Left",           L"RotateLeft");
    HK_DEF(i++, HK_ROTATE_RIGHT,      MOD_SHIFT,             VK_OEM_PERIOD,HKSCOPE_LOCAL, HKCAT_VISUAL, L"Rotate Right",          L"RotateRight");
    HK_DEF(i++, HK_BRIGHTNESS_DOWN,   0,                     VK_OEM_MINUS, HKSCOPE_LOCAL, HKCAT_VISUAL, L"Brightness -",          L"BrightnessDown");
    HK_DEF(i++, HK_BRIGHTNESS_UP,     MOD_SHIFT,             VK_OEM_PLUS,  HKSCOPE_LOCAL, HKCAT_VISUAL, L"Brightness +",          L"BrightnessUp");
    HK_DEF(i++, HK_HUE_FORWARD,       MOD_CONTROL,           'H',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Hue +",                 L"HueForward");
    HK_DEF(i++, HK_HUE_BACKWARD,      MOD_CONTROL|MOD_SHIFT, 'H',          HKSCOPE_LOCAL, HKCAT_VISUAL, L"Hue -",                 L"HueBackward");

    // ── Media ──
    HK_DEF(i++, HK_MEDIA_PLAY_PAUSE,  0,                     VK_DOWN,      HKSCOPE_LOCAL, HKCAT_MEDIA, L"Play/Pause",            L"MediaPlayPause");
    HK_DEF(i++, HK_MEDIA_STOP,        0,                     VK_UP,        HKSCOPE_LOCAL, HKCAT_MEDIA, L"Stop",                  L"MediaStop");
    HK_DEF(i++, HK_MEDIA_PREV_TRACK,  0,                     VK_LEFT,      HKSCOPE_LOCAL, HKCAT_MEDIA, L"Previous Track",        L"MediaPrevTrack");
    HK_DEF(i++, HK_MEDIA_NEXT_TRACK,  0,                     VK_RIGHT,     HKSCOPE_LOCAL, HKCAT_MEDIA, L"Next Track",            L"MediaNextTrack");
    HK_DEF(i++, HK_MEDIA_REWIND,      MOD_CONTROL,           VK_LEFT,      HKSCOPE_LOCAL, HKCAT_MEDIA, L"Rewind",                L"MediaRewind");
    HK_DEF(i++, HK_MEDIA_FAST_FORWARD,MOD_CONTROL,           VK_RIGHT,     HKSCOPE_LOCAL, HKCAT_MEDIA, L"Fast Forward",          L"MediaFastFwd");

    // ── Window ──
    HK_DEF(i++, HK_TOGGLE_FULLSCREEN, 0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Toggle Fullscreen",     L"ToggleFullscreen");
    HK_DEF(i++, HK_TOGGLE_STRETCH,    0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Toggle Stretch/Mirror", L"ToggleStretch");
    HK_DEF(i++, HK_MIRROR_ONLY,      0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Toggle Mirror",         L"ToggleMirror");
    HK_DEF(i++, HK_STRETCH_ONLY,     0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Toggle Stretch",        L"ToggleStretchOnly");
    HK_DEF(i++, HK_ALWAYS_ON_TOP,     0,                     VK_F7,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Always On Top",         L"AlwaysOnTop");
    HK_DEF(i++, HK_TRANSPARENCY_MODE, 0,                     VK_F12,       HKSCOPE_LOCAL, HKCAT_WINDOW, L"Transparency Mode",     L"TransparencyMode");
    HK_DEF(i++, HK_BLACK_MODE,        MOD_CONTROL,           VK_F12,       HKSCOPE_LOCAL, HKCAT_WINDOW, L"Black Mode",            L"BlackMode");
    HK_DEF(i++, HK_FPS_CYCLE,         0,                     VK_F3,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"FPS Cycle",             L"FPSCycle");
    HK_DEF(i++, HK_SHOW_PRESET_INFO,  0,                     VK_F4,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Show Preset Info",      L"ShowPresetInfo");
    HK_DEF(i++, HK_SHOW_FPS,          0,                     VK_F5,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Show FPS",              L"ShowFPS");
    HK_DEF(i++, HK_SHOW_RATING,       0,                     VK_F6,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Show Rating",           L"ShowRating");
    HK_DEF(i++, HK_SHOW_SHADER_HELP,  0,                     VK_F9,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Shader Help",           L"ShowShaderHelp");

    // ── Tools ──
    HK_DEF(i++, HK_OPEN_SETTINGS,     0,                     VK_F8,        HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Settings",         L"OpenSettings");
    HK_DEF(i++, HK_OPEN_DISPLAYS,     MOD_CONTROL,           VK_F8,        HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Spout/Displays",   L"OpenDisplays");
    HK_DEF(i++, HK_OPEN_SONGINFO,     MOD_SHIFT|MOD_CONTROL, VK_F8,        HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Song Info",        L"OpenSongInfo");
    HK_DEF(i++, HK_OPEN_HOTKEYS,      MOD_CONTROL,           VK_F7,        HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Hotkeys",          L"OpenHotkeys");
    HK_DEF(i++, HK_OPEN_MIDI,         0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open MIDI",             L"OpenMidi");
    HK_DEF(i++, HK_OPEN_BOARD,        0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Button Board",     L"OpenBoard");
    HK_DEF(i++, HK_OPEN_PRESETS,      0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Presets",          L"OpenPresets");
    HK_DEF(i++, HK_OPEN_SPRITES,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Sprites",          L"OpenSprites");
    HK_DEF(i++, HK_OPEN_MESSAGES,    0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Messages",         L"OpenMessages");
    HK_DEF(i++, HK_OPEN_SHADER_IMPORT,0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Shader Import",    L"OpenShaderImport");
    HK_DEF(i++, HK_OPEN_VIDEO_FX,    0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Video Effects",    L"OpenVideoFX");
    HK_DEF(i++, HK_OPEN_VFX_PROFILES,0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open VFX Profiles",    L"OpenVFXProfiles");
    HK_DEF(i++, HK_OPEN_WORKSPACE_LAYOUT,0,               0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Workspace Layout", L"OpenWorkspaceLayout");
    HK_DEF(i++, HK_APPLY_WORKSPACE_LAYOUT,0,              0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Apply Workspace Layout", L"ApplyWorkspaceLayout");
    HK_DEF(i++, HK_OPEN_TEXT_ANIM,   0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Text Animations",  L"OpenTextAnim");
    HK_DEF(i++, HK_OPEN_REMOTE,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Remote",           L"OpenRemote");
    HK_DEF(i++, HK_OPEN_VISUAL,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Visual",           L"OpenVisual");
    HK_DEF(i++, HK_OPEN_COLORS,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Colors",           L"OpenColors");
    HK_DEF(i++, HK_OPEN_CONTROLLER, 0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Controller",       L"OpenController");
    HK_DEF(i++, HK_OPEN_ANNOTATIONS,0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Annotations",      L"OpenAnnotations");
    HK_DEF(i++, HK_OPEN_SCRIPT,    0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Script",           L"OpenScript");
    HK_DEF(i++, HK_POLL_TRACK_INFO, 0,                     0,            HKSCOPE_LOCAL, HKCAT_MEDIA, L"Poll Track Info",       L"PollTrackInfo");
    HK_DEF(i++, HK_MIRROR_WATERMARK,0,                    0,            HKSCOPE_LOCAL, HKCAT_WINDOW,L"Mirror Watermark",      L"MirrorWatermark");

    // ── Shader/Effects ──
    HK_DEF(i++, HK_INJECT_EFFECT_CYCLE, 0,                   VK_F11,       HKSCOPE_LOCAL, HKCAT_SHADER, L"Inject Effect Cycle",  L"InjectEffectCycle");
    HK_DEF(i++, HK_HARDCUT_MODE_CYCLE,MOD_SHIFT,             VK_F11,       HKSCOPE_LOCAL, HKCAT_SHADER, L"Hard Cut Mode Cycle",  L"HardcutModeCycle");
    HK_DEF(i++, HK_QUALITY_DOWN,      MOD_CONTROL,           'Q',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Quality Down",          L"QualityDown");
    HK_DEF(i++, HK_QUALITY_UP,        MOD_CONTROL|MOD_SHIFT, 'Q',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Quality Up",            L"QualityUp");
    HK_DEF(i++, HK_SPOUT_TOGGLE,      0,                     VK_F10,       HKSCOPE_LOCAL, HKCAT_SHADER, L"Spout Toggle",          L"SpoutToggle");
    HK_DEF(i++, HK_SPOUT_FIXED_SIZE,  MOD_SHIFT,             VK_F10,       HKSCOPE_LOCAL, HKCAT_SHADER, L"Spout Fixed Size",     L"SpoutFixedSize");
    HK_DEF(i++, HK_SCREENSHOT,        MOD_CONTROL,           'X',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Screenshot",            L"Screenshot");
    HK_DEF(i++, HK_SHADER_LOCK_CYCLE, 0,                     'D',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Shader Lock Cycle",    L"ShaderLockCycle");
    HK_DEF(i++, HK_SONG_TITLE,        0,                     'T',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Song Title Anim",      L"SongTitle");
    HK_DEF(i++, HK_KILL_SPRITES,      MOD_CONTROL,           'K',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Kill Sprites",          L"KillSprites");
    HK_DEF(i++, HK_KILL_SUPERTEXTS,   MOD_CONTROL,           'T',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Kill Text Overlays",   L"KillSupertexts");
    HK_DEF(i++, HK_AUTO_PRESET_CHANGE,MOD_CONTROL,           'A',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Auto Preset Change",   L"AutoPresetChange");
    HK_DEF(i++, HK_SCRAMBLE_WARP,     MOD_SHIFT,             '1',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Scramble Warp",        L"ScrambleWarp");
    HK_DEF(i++, HK_SCRAMBLE_COMP,     MOD_SHIFT,             '2',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Scramble Comp",        L"ScrambleComp");
    HK_DEF(i++, HK_QUICKSAVE,         MOD_CONTROL,           'S',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Quicksave Preset",     L"Quicksave");
    HK_DEF(i++, HK_SCROLL_LOCK,       0,                     VK_SCROLL,    HKSCOPE_LOCAL, HKCAT_SHADER, L"Scroll Lock",          L"ScrollLock");
    HK_DEF(i++, HK_RELOAD_MESSAGES,   MOD_SHIFT,             '8',          HKSCOPE_LOCAL, HKCAT_SHADER, L"Reload Messages",      L"ReloadMessages");

    // ── Misc ──
    HK_DEF(i++, HK_DEBUG_INFO,        0,                     'N',          HKSCOPE_LOCAL, HKCAT_MISC, L"Debug Info",              L"DebugInfo");
    HK_DEF(i++, HK_SPRITE_MODE,       0,                     'K',          HKSCOPE_LOCAL, HKCAT_MISC, L"Sprite/Message Mode",    L"SpriteMode");

    // User-added hotkeys (Script/Launch) are NOT reset here — they're user-created.
}

#undef HK_DEF
#undef HK_DEF2

void Engine::LoadHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();

    // Start from defaults
    ResetHotkeyDefaults();

    // Version marker:
    // 0 (absent) = pre-expansion (only 10 hotkeys)
    // 2 = full reassignable hotkeys (fixed Script/Launch slots)
    // 3 = dynamic user hotkeys (Script/Launch replaced by vector)
    // 4 = dual local/global bindings per action
    static constexpr int HOTKEY_INI_VERSION = 4;
    int iniVersion = GetPrivateProfileIntW(L"Hotkeys", L"Version", 0, pIni);

    // Migration: if old "Enabled" key exists, migrate scope for configured bindings
    int oldEnabled = GetPrivateProfileIntW(L"Hotkeys", L"Enabled", -1, pIni);

    if (iniVersion >= 2) {
        // Read built-in hotkey overrides from INI
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            wchar_t modKey[128], vkKey[128], scopeKey[128];
            swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
            swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
            swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

            m_hotkeys[i].modifiers = (UINT)GetPrivateProfileIntW(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers, pIni);
            m_hotkeys[i].vk = (UINT)GetPrivateProfileIntW(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk, pIni);
            m_hotkeys[i].scope = (HotkeyScope)GetPrivateProfileIntW(L"Hotkeys", scopeKey, (int)m_hotkeys[i].scope, pIni);

            if (iniVersion >= 4) {
                // Version 4+: read global binding
                wchar_t gModKey[128], gVkKey[128];
                swprintf(gModKey, 128, L"%s_GlobalMod", m_hotkeys[i].szIniKey);
                swprintf(gVkKey, 128, L"%s_GlobalVK", m_hotkeys[i].szIniKey);
                m_hotkeys[i].globalMod = (UINT)GetPrivateProfileIntW(L"Hotkeys", gModKey, (int)m_hotkeys[i].globalMod, pIni);
                m_hotkeys[i].globalVK = (UINT)GetPrivateProfileIntW(L"Hotkeys", gVkKey, (int)m_hotkeys[i].globalVK, pIni);
            }
        }

        // Migration from version 2/3: if scope was GLOBAL, move binding to global slot
        if (iniVersion < 4) {
            for (int i = 0; i < NUM_HOTKEYS; i++) {
                if (m_hotkeys[i].scope == HKSCOPE_GLOBAL && m_hotkeys[i].vk != 0) {
                    m_hotkeys[i].globalMod = m_hotkeys[i].modifiers;
                    m_hotkeys[i].globalVK = m_hotkeys[i].vk;
                    m_hotkeys[i].modifiers = m_hotkeys[i].defaultMod;
                    m_hotkeys[i].vk = m_hotkeys[i].defaultVK;
                    m_hotkeys[i].scope = HKSCOPE_LOCAL;
                }
            }
        }
    } else {
        // Pre-expansion INI: only load bindings that existed in the old system
        static const wchar_t* oldKeys[] = {
            L"ToggleFullscreen", L"ToggleStretch", L"OpenSettings", L"OpenDisplays",
            L"OpenSongInfo", L"OpenHotkeys"
        };
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            bool isOldKey = false;
            for (auto* k : oldKeys) {
                if (wcscmp(m_hotkeys[i].szIniKey, k) == 0) { isOldKey = true; break; }
            }
            if (!isOldKey) continue;

            wchar_t modKey[128], vkKey[128], scopeKey[128];
            swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
            swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
            swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

            m_hotkeys[i].modifiers = (UINT)GetPrivateProfileIntW(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers, pIni);
            m_hotkeys[i].vk = (UINT)GetPrivateProfileIntW(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk, pIni);
            m_hotkeys[i].scope = (HotkeyScope)GetPrivateProfileIntW(L"Hotkeys", scopeKey, (int)m_hotkeys[i].scope, pIni);
        }
    }

    // Migration: old system had master enable toggle for first 2 bindings
    if (oldEnabled >= 0) {
        if (oldEnabled == 1) {
            for (int i = 0; i < NUM_HOTKEYS; i++) {
                if ((m_hotkeys[i].id == HK_TOGGLE_FULLSCREEN || m_hotkeys[i].id == HK_TOGGLE_STRETCH)
                    && m_hotkeys[i].vk != 0)
                    m_hotkeys[i].scope = HKSCOPE_GLOBAL;
            }
        }
        WritePrivateProfileStringW(L"Hotkeys", L"Enabled", NULL, pIni);
    }

    // Version 2→3 cleanup: remove old fixed Script/Launch keys
    if (iniVersion == 2) {
        for (int i = 1; i <= 10; i++) {
            wchar_t key[64];
            swprintf(key, 64, L"Script%d_Cmd", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"Script%d_Mod", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"Script%d_VK", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"Script%d_Scope", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
        }
        for (int i = 1; i <= 4; i++) {
            wchar_t key[64];
            swprintf(key, 64, L"LaunchApp%d_Path", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"LaunchApp%d_Mod", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"LaunchApp%d_VK", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
            swprintf(key, 64, L"LaunchApp%d_Scope", i);
            WritePrivateProfileStringW(L"Hotkeys", key, NULL, pIni);
        }
    }

    // Load dynamic user hotkeys (Version 3+)
    m_userHotkeys.clear();
    if (iniVersion >= 3) {
        int count = GetPrivateProfileIntW(L"Hotkeys", L"UserHotkey_Count", 0, pIni);
        m_nextUserHotkeyId = GetPrivateProfileIntW(L"Hotkeys", L"UserHotkey_NextID", USER_HOTKEY_ID_BASE, pIni);

        for (int i = 0; i < count; i++) {
            wchar_t prefix[64];
            swprintf(prefix, 64, L"UserHotkey_%d_", i);

            UserHotkey uh;
            wchar_t keyBuf[128], valBuf[512];

            swprintf(keyBuf, 128, L"%sID", prefix);
            uh.id = GetPrivateProfileIntW(L"Hotkeys", keyBuf, USER_HOTKEY_ID_BASE + i, pIni);

            swprintf(keyBuf, 128, L"%sType", prefix);
            uh.type = (UserHotkeyType)GetPrivateProfileIntW(L"Hotkeys", keyBuf, 0, pIni);

            swprintf(keyBuf, 128, L"%sMod", prefix);
            uh.modifiers = (UINT)GetPrivateProfileIntW(L"Hotkeys", keyBuf, 0, pIni);

            swprintf(keyBuf, 128, L"%sVK", prefix);
            uh.vk = (UINT)GetPrivateProfileIntW(L"Hotkeys", keyBuf, 0, pIni);

            swprintf(keyBuf, 128, L"%sScope", prefix);
            uh.scope = (HotkeyScope)GetPrivateProfileIntW(L"Hotkeys", keyBuf, 0, pIni);

            swprintf(keyBuf, 128, L"%sLabel", prefix);
            GetPrivateProfileStringW(L"Hotkeys", keyBuf, L"", valBuf, 512, pIni);
            uh.label = valBuf;

            swprintf(keyBuf, 128, L"%sCmd", prefix);
            GetPrivateProfileStringW(L"Hotkeys", keyBuf, L"", valBuf, 512, pIni);
            uh.command = valBuf;

            m_userHotkeys.push_back(std::move(uh));
        }
    }

    // Save as Version 3 if upgrading
    if (iniVersion < HOTKEY_INI_VERSION)
        SaveHotkeySettings();
}

void Engine::SaveHotkeySettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    // Write version marker
    WritePrivateProfileStringW(L"Hotkeys", L"Version", L"4", pIni);

    // Save built-in hotkey bindings
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        wchar_t modKey[128], vkKey[128], scopeKey[128];
        swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
        swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
        swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

        swprintf(buf, 64, L"%u", m_hotkeys[i].modifiers);
        WritePrivateProfileStringW(L"Hotkeys", modKey, buf, pIni);
        swprintf(buf, 64, L"%u", m_hotkeys[i].vk);
        WritePrivateProfileStringW(L"Hotkeys", vkKey, buf, pIni);
        swprintf(buf, 64, L"%d", (int)m_hotkeys[i].scope);
        WritePrivateProfileStringW(L"Hotkeys", scopeKey, buf, pIni);

        // Global binding
        wchar_t gModKey[128], gVkKey[128];
        swprintf(gModKey, 128, L"%s_GlobalMod", m_hotkeys[i].szIniKey);
        swprintf(gVkKey, 128, L"%s_GlobalVK", m_hotkeys[i].szIniKey);
        swprintf(buf, 64, L"%u", m_hotkeys[i].globalMod);
        WritePrivateProfileStringW(L"Hotkeys", gModKey, buf, pIni);
        swprintf(buf, 64, L"%u", m_hotkeys[i].globalVK);
        WritePrivateProfileStringW(L"Hotkeys", gVkKey, buf, pIni);
    }

    // Save dynamic user hotkeys
    int count = (int)m_userHotkeys.size();
    swprintf(buf, 64, L"%d", count);
    WritePrivateProfileStringW(L"Hotkeys", L"UserHotkey_Count", buf, pIni);
    swprintf(buf, 64, L"%d", m_nextUserHotkeyId);
    WritePrivateProfileStringW(L"Hotkeys", L"UserHotkey_NextID", buf, pIni);

    for (int i = 0; i < count; i++) {
        const auto& uh = m_userHotkeys[i];
        wchar_t prefix[64], keyBuf[128];
        swprintf(prefix, 64, L"UserHotkey_%d_", i);

        swprintf(keyBuf, 128, L"%sID", prefix);
        swprintf(buf, 64, L"%d", uh.id);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, buf, pIni);

        swprintf(keyBuf, 128, L"%sType", prefix);
        swprintf(buf, 64, L"%d", (int)uh.type);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, buf, pIni);

        swprintf(keyBuf, 128, L"%sMod", prefix);
        swprintf(buf, 64, L"%u", uh.modifiers);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, buf, pIni);

        swprintf(keyBuf, 128, L"%sVK", prefix);
        swprintf(buf, 64, L"%u", uh.vk);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, buf, pIni);

        swprintf(keyBuf, 128, L"%sScope", prefix);
        swprintf(buf, 64, L"%d", (int)uh.scope);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, buf, pIni);

        swprintf(keyBuf, 128, L"%sLabel", prefix);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, uh.label.c_str(), pIni);

        swprintf(keyBuf, 128, L"%sCmd", prefix);
        WritePrivateProfileStringW(L"Hotkeys", keyBuf, uh.command.c_str(), pIni);
    }

    // Clean up any stale user hotkey entries beyond current count
    for (int i = count; i < count + 20; i++) {
        wchar_t keyBuf[128];
        swprintf(keyBuf, 128, L"UserHotkey_%d_ID", i);
        wchar_t test[16];
        GetPrivateProfileStringW(L"Hotkeys", keyBuf, L"", test, 16, pIni);
        if (test[0] == L'\0') break;  // no more stale entries
        // Delete all fields for this stale entry
        wchar_t prefix[64];
        swprintf(prefix, 64, L"UserHotkey_%d_", i);
        for (auto* suffix : { L"ID", L"Type", L"Mod", L"VK", L"Scope", L"Label", L"Cmd" }) {
            swprintf(keyBuf, 128, L"%s%s", prefix, suffix);
            WritePrivateProfileStringW(L"Hotkeys", keyBuf, NULL, pIni);
        }
    }
}

// ── Help display category order ──

void Engine::ResetHelpCatOrder()
{
    // Default order: Window, Navigation, Tools, Visual, Misc, Launch, Media, Shader, Script
    const int defaultOrder[] = {
        HKCAT_WINDOW, HKCAT_NAVIGATION, HKCAT_TOOLS, HKCAT_VISUAL,
        HKCAT_MISC, HKCAT_LAUNCH, HKCAT_MEDIA, HKCAT_SHADER, HKCAT_SCRIPT
    };
    for (int i = 0; i < HKCAT_COUNT; i++)
        m_helpCatOrder[i] = defaultOrder[i];
}

void Engine::LoadHelpCatOrder()
{
    ResetHelpCatOrder();
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[256] = {};
    GetPrivateProfileStringW(L"Hotkeys", L"HelpCatOrder", L"", buf, 256, pIni);
    if (buf[0] == L'\0') return;

    // Parse comma-separated ints
    int order[HKCAT_COUNT];
    int count = 0;
    wchar_t* ctx = nullptr;
    wchar_t* tok = wcstok_s(buf, L",", &ctx);
    while (tok && count < HKCAT_COUNT) {
        int v = _wtoi(tok);
        if (v >= 0 && v < HKCAT_COUNT) order[count++] = v;
        tok = wcstok_s(nullptr, L",", &ctx);
    }
    if (count == HKCAT_COUNT) {
        // Validate: must contain each category exactly once
        bool seen[HKCAT_COUNT] = {};
        bool valid = true;
        for (int i = 0; i < HKCAT_COUNT; i++) {
            if (seen[order[i]]) { valid = false; break; }
            seen[order[i]] = true;
        }
        if (valid)
            memcpy(m_helpCatOrder, order, sizeof(m_helpCatOrder));
    }
}

void Engine::SaveHelpCatOrder()
{
    wchar_t buf[256] = {};
    wchar_t* p = buf;
    for (int i = 0; i < HKCAT_COUNT; i++) {
        if (i > 0) *p++ = L',';
        p += swprintf(p, 16, L"%d", m_helpCatOrder[i]);
    }
    WritePrivateProfileStringW(L"Hotkeys", L"HelpCatOrder", buf, GetConfigIniFile());
}

void Engine::RegisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].globalVK != 0)
            RegisterHotKey(hwnd, m_hotkeys[i].id, m_hotkeys[i].globalMod | MOD_NOREPEAT, m_hotkeys[i].globalVK);
    }
    for (const auto& uh : m_userHotkeys) {
        if (uh.vk != 0 && uh.scope == HKSCOPE_GLOBAL)
            RegisterHotKey(hwnd, uh.id, uh.modifiers | MOD_NOREPEAT, uh.vk);
    }
}

void Engine::UnregisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;
    for (int i = 0; i < NUM_HOTKEYS; i++)
        UnregisterHotKey(hwnd, m_hotkeys[i].id);
    for (const auto& uh : m_userHotkeys)
        UnregisterHotKey(hwnd, uh.id);
}

bool Engine::DispatchHotkeyAction(int actionId)
{
    HWND hRender = GetPluginWindow();
    #define clamp(value, mn, mx) ((value) < (mn) ? (mn) : ((value) > (mx) ? (mx) : (value)))

    switch (actionId) {
    // ── Navigation ──
    case HK_NEXT_PRESET:
        if (!m_bPresetLockedByCode) {
            RenderCommand cmd;
            cmd.cmd = RenderCmd::NextPreset;
            cmd.fParam = m_fBlendTimeUser;
            EnqueueRenderCmd(std::move(cmd));
        }
        return true;
    case HK_PREV_PRESET:
        PrevPreset(0);
        m_fHardCutThresh *= 2.0f;
        return true;
    case HK_HARD_CUT:
        NextPreset(0);
        m_fHardCutThresh *= 2.0f;
        return true;
    case HK_RANDOM_MASHUP: {
        bool bCompLock = m_bCompShaderLock;
        bool bWarpLock = m_bWarpShaderLock;
        m_bCompShaderLock = false; m_bWarpShaderLock = false;
        LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bCompShaderLock = true; m_bWarpShaderLock = false;
            LoadRandomPreset(0.0f);
            if (WaitForPendingLoad(3000)) {
                m_bCompShaderLock = false; m_bWarpShaderLock = true;
                LoadRandomPreset(0.0f);
                WaitForPendingLoad(3000);
            }
        }
        m_bCompShaderLock = bCompLock;
        m_bWarpShaderLock = bWarpLock;
        return true;
    }
    case HK_LOCK_PRESET:
        m_bPresetLockedByUser = !m_bPresetLockedByUser;
        AddNotification(m_bPresetLockedByUser ? L"Preset locked" : L"Preset unlocked");
        SendSettingsInfoToMDropDX12Remote();
        return true;
    case HK_TOGGLE_RANDOM:
        m_bSequentialPresetOrder = !m_bSequentialPresetOrder;
        AddNotification(m_bSequentialPresetOrder ? L"Preset order: Sequential" : L"Preset order: Random");
        m_presetHistory[0] = m_szCurrentPresetFile;
        m_presetHistoryPos = 0;
        m_presetHistoryFwdFence = 1;
        m_presetHistoryBackFence = 0;
        return true;
    case HK_OPEN_PRESET_LIST:
        m_show_help = 0;
        if (m_UI_mode == UI_LOAD) {
            m_UI_mode = UI_REGULAR;
        } else if (m_UI_mode == UI_REGULAR || m_UI_mode == UI_MENU) {
            if (!DirHasMilkFilesHelper(m_szPresetDir)) {
                swprintf(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);
                TryDescendIntoPresetSubdirHelper(m_szPresetDir);
                WritePrivateProfileStringW(L"Settings", L"szPresetDir", m_szPresetDir, GetConfigIniFile());
            }
            UpdatePresetList(true, true);
            m_UI_mode = UI_LOAD;
            m_bUserPagedUp = false;
            m_bUserPagedDown = false;
        }
        return true;
    case HK_SAVE_PRESET:
        if (m_UI_mode == UI_REGULAR) {
            m_show_help = 0;
            m_UI_mode = UI_SAVEAS;
            m_waitstring.bActive = true;
            m_waitstring.bFilterBadChars = true;
            m_waitstring.bDisplayAsCode = false;
            m_waitstring.nSelAnchorPos = -1;
            m_waitstring.nMaxLen = min(sizeof(m_waitstring.szText) - 1,
                (size_t)(MAX_PATH - lstrlenW(GetPresetDir()) - 6));
            lstrcpyW(m_waitstring.szText, m_pState->m_szDesc);
            wasabiApiLangString(IDS_SAVE_AS, m_waitstring.szPrompt, 512);
            m_waitstring.szToolTip[0] = 0;
            m_waitstring.nCursorPos = lstrlenW(m_waitstring.szText);
        }
        return true;
    case HK_OPEN_MENU:
        m_show_help = 0;
        if (m_UI_mode == UI_MENU)
            m_UI_mode = UI_REGULAR;
        else if (m_UI_mode == UI_REGULAR || m_UI_mode == UI_LOAD)
            m_UI_mode = UI_MENU;
        return true;

    // ── Visual ──
    case HK_OPACITY_UP:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_OPACITY_DOWN:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_OPACITY_25:  fOpacity = 0.25f; if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0); return true;
    case HK_OPACITY_50:  fOpacity = 0.50f; if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0); return true;
    case HK_OPACITY_75:  fOpacity = 0.75f; if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0); return true;
    case HK_OPACITY_100: fOpacity = 1.00f; if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0); return true;
    case HK_WAVE_MODE_NEXT:
        m_pState->m_nWaveMode++;
        if (m_pState->m_nWaveMode >= NUM_WAVES) m_pState->m_nWaveMode = 0;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WAVE_MODE_PREV:
        m_pState->m_nWaveMode--;
        if (m_pState->m_nWaveMode < 0) m_pState->m_nWaveMode = NUM_WAVES - 1;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WAVE_ALPHA_DOWN:
        m_pState->m_fWaveAlpha -= 0.1f;
        if (m_pState->m_fWaveAlpha.eval(-1) < 0.0f) m_pState->m_fWaveAlpha = 0.0f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WAVE_ALPHA_UP:
        m_pState->m_fWaveAlpha += 0.1f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WAVE_SCALE_DOWN:
        m_pState->m_fWaveScale *= 0.9f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WAVE_SCALE_UP:
        m_pState->m_fWaveScale /= 0.9f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_ZOOM_IN:
        m_pState->m_fZoom += 0.01f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_ZOOM_OUT:
        m_pState->m_fZoom -= 0.01f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WARP_AMOUNT_DOWN:
        m_pState->m_fWarpAmount /= 1.1f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WARP_AMOUNT_UP:
        m_pState->m_fWarpAmount *= 1.1f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_WARP_SCALE_DOWN:
        m_pState->m_fWarpScale /= 1.1f;
        return true;
    case HK_WARP_SCALE_UP:
        m_pState->m_fWarpScale *= 1.1f;
        return true;
    case HK_ECHO_ALPHA_DOWN:
        m_pState->m_fVideoEchoAlpha -= 0.1f;
        if (m_pState->m_fVideoEchoAlpha.eval(-1) < 0) m_pState->m_fVideoEchoAlpha = 0;
        return true;
    case HK_ECHO_ALPHA_UP:
        m_pState->m_fVideoEchoAlpha += 0.1f;
        if (m_pState->m_fVideoEchoAlpha.eval(-1) > 1.0f) m_pState->m_fVideoEchoAlpha = 1.0f;
        return true;
    case HK_ECHO_ZOOM_DOWN:
        m_pState->m_fVideoEchoZoom /= 1.05f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_ECHO_ZOOM_UP:
        m_pState->m_fVideoEchoZoom *= 1.05f;
        SendPresetWaveInfoToMDropDX12Remote();
        return true;
    case HK_ECHO_ORIENT:
        m_pState->m_nVideoEchoOrientation = (m_pState->m_nVideoEchoOrientation + 1) % 4;
        return true;
    case HK_GAMMA_DOWN:
        m_pState->m_fGammaAdj -= 0.1f;
        if (m_pState->m_fGammaAdj.eval(-1) < 0.0f) m_pState->m_fGammaAdj = 0.0f;
        { wchar_t buf[64]; swprintf(buf, 64, L"Gamma: %.1f", m_pState->m_fGammaAdj.eval(-1));
          AddNotificationColored(buf, 1.5f, 0xFF00FFFF); }
        return true;
    case HK_GAMMA_UP:
        m_pState->m_fGammaAdj += 0.1f;
        { wchar_t buf[64]; swprintf(buf, 64, L"Gamma: %.1f", m_pState->m_fGammaAdj.eval(-1));
          AddNotificationColored(buf, 1.5f, 0xFF00FFFF); }
        return true;
    case HK_PUSH_X_NEG: m_pState->m_fXPush -= 0.005f; return true;
    case HK_PUSH_X_POS: m_pState->m_fXPush += 0.005f; return true;
    case HK_PUSH_Y_NEG: m_pState->m_fYPush -= 0.005f; return true;
    case HK_PUSH_Y_POS: m_pState->m_fYPush += 0.005f; return true;
    case HK_ROTATE_LEFT:  m_pState->m_fRot += 0.02f; return true;
    case HK_ROTATE_RIGHT: m_pState->m_fRot -= 0.02f; return true;
    case HK_BRIGHTNESS_DOWN:
        m_ColShiftBrightness -= 0.02f;
        if (m_ColShiftBrightness < -1.0f) m_ColShiftBrightness = -1.0f;
        { wchar_t buf[64]; swprintf(buf, 64, L"Brightness: %.2f", m_ColShiftBrightness);
          AddNotificationColored(buf, 1.5f, 0xFF00FFFF); }
        SendSettingsInfoToMDropDX12Remote();
        return true;
    case HK_BRIGHTNESS_UP:
        m_ColShiftBrightness += 0.02f;
        if (m_ColShiftBrightness > 1.0f) m_ColShiftBrightness = 1.0f;
        { wchar_t buf[64]; swprintf(buf, 64, L"Brightness: %.2f", m_ColShiftBrightness);
          AddNotificationColored(buf, 1.5f, 0xFF00FFFF); }
        SendSettingsInfoToMDropDX12Remote();
        return true;
    case HK_HUE_FORWARD:
        m_ColShiftHue += 0.02f;
        if (m_ColShiftHue >= 1.0f) m_ColShiftHue = -1.0f;
        SendSettingsInfoToMDropDX12Remote();
        return true;
    case HK_HUE_BACKWARD:
        m_ColShiftHue -= 0.02f;
        if (m_ColShiftHue <= -1.0f) m_ColShiftHue = 1.0f;
        SendSettingsInfoToMDropDX12Remote();
        return true;

    // ── Media (need render window for keybd_event context) ──
    case HK_MEDIA_PLAY_PAUSE:
    case HK_MEDIA_STOP:
    case HK_MEDIA_PREV_TRACK:
    case HK_MEDIA_NEXT_TRACK:
    case HK_MEDIA_REWIND:
    case HK_MEDIA_FAST_FORWARD:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;

    // ── Window (need App.cpp or render-window-local state) ──
    case HK_TOGGLE_FULLSCREEN:
    case HK_TOGGLE_STRETCH:
    case HK_MIRROR_ONLY:
    case HK_STRETCH_ONLY:
    case HK_ALWAYS_ON_TOP:
    case HK_TRANSPARENCY_MODE:
    case HK_BLACK_MODE:
    case HK_FPS_CYCLE:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_SHOW_PRESET_INFO:
        m_bShowPresetInfo = !m_bShowPresetInfo;
        return true;
    case HK_SHOW_FPS:
        m_bShowFPS = !m_bShowFPS;
        return true;
    case HK_SHOW_RATING:
        m_bShowRating = !m_bShowRating;
        return true;
    case HK_SHOW_SHADER_HELP:
        m_bShowShaderHelp = !m_bShowShaderHelp;
        return true;

    // ── Tools ──
    case HK_OPEN_SETTINGS:
        OpenSettingsWindow();
        return true;
    case HK_OPEN_DISPLAYS:
        OpenDisplaysWindow();
        return true;
    case HK_OPEN_SONGINFO:
        OpenSongInfoWindow();
        return true;
    case HK_OPEN_HOTKEYS:
        OpenHotkeysWindow();
        return true;
    case HK_OPEN_MIDI:
        OpenMidiWindow();
        return true;
    case HK_OPEN_BOARD:
        OpenBoardWindow();
        return true;
    case HK_OPEN_PRESETS:
        OpenPresetsWindow();
        return true;
    case HK_OPEN_SPRITES:
        OpenSpritesWindow();
        return true;
    case HK_OPEN_MESSAGES:
        OpenMessagesWindow();
        return true;
    case HK_OPEN_SHADER_IMPORT:
        OpenShaderImportWindow();
        return true;
    case HK_OPEN_VIDEO_FX:
        OpenVideoEffectsWindow();
        return true;
    case HK_OPEN_VFX_PROFILES:
        OpenVFXProfileWindow();
        return true;
    case HK_OPEN_WORKSPACE_LAYOUT:
        OpenWorkspaceLayoutWindow();
        return true;
    case HK_APPLY_WORKSPACE_LAYOUT:
        if (m_workspaceLayoutWindow && m_workspaceLayoutWindow->IsOpen()) {
            m_workspaceLayoutWindow->ApplyLayout();
        } else {
            if (!m_workspaceLayoutWindow)
                m_workspaceLayoutWindow = std::make_unique<WorkspaceLayoutWindow>(this);
            m_workspaceLayoutWindow->SetAutoApply();
            m_workspaceLayoutWindow->Open();
        }
        return true;
    case HK_OPEN_TEXT_ANIM:
        OpenTextAnimWindow();
        return true;
    case HK_OPEN_REMOTE:
        OpenRemoteWindow();
        return true;
    case HK_OPEN_VISUAL:
        OpenVisualWindow();
        return true;
    case HK_OPEN_COLORS:
        OpenColorsWindow();
        return true;
    case HK_OPEN_CONTROLLER:
        OpenControllerWindow();
        return true;
    case HK_OPEN_ANNOTATIONS:
        OpenAnnotationsWindow();
        return true;
    case HK_OPEN_SCRIPT:
        OpenScriptWindow();
        return true;
    case HK_POLL_TRACK_INFO:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_MIRROR_WATERMARK:
        if (hRender) PostMessage(hRender, WM_MW_MIRROR_WM, 0, 0);
        return true;

    // ── Shader/Effects ──
    case HK_INJECT_EFFECT_CYCLE:
    case HK_HARDCUT_MODE_CYCLE:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_QUALITY_DOWN: {
        float newQuality = clamp(m_fRenderQuality * 0.5f, 0.01f, 1.0f);
        if (fabsf(newQuality - m_fRenderQuality) > 0.0001f) {
            m_fRenderQuality = newQuality;
            EnqueueRenderCmd(RenderCmd::ResetBuffers);
            SendSettingsInfoToMDropDX12Remote();
        }
        return true;
    }
    case HK_QUALITY_UP: {
        float newQuality = clamp(m_fRenderQuality * 2.0f, 0.01f, 1.0f);
        if (fabsf(newQuality - m_fRenderQuality) > 0.0001f) {
            m_fRenderQuality = newQuality;
            EnqueueRenderCmd(RenderCmd::ResetBuffers);
            SendSettingsInfoToMDropDX12Remote();
        }
        return true;
    }
    case HK_SPOUT_TOGGLE:
        ToggleSpout();
        return true;
    case HK_SPOUT_FIXED_SIZE:
        SetSpoutFixedSize(true, true);
        return true;
    case HK_SCREENSHOT:
        EnqueueRenderCmd(RenderCmd::CaptureScreenshot);
        return true;
    case HK_SHADER_LOCK_CYCLE:
        if (!m_bCompShaderLock && !m_bWarpShaderLock) {
            m_bCompShaderLock = true; m_bWarpShaderLock = false;
            AddNotification(L"Comp shader locked");
        } else if (m_bCompShaderLock && !m_bWarpShaderLock) {
            m_bCompShaderLock = false; m_bWarpShaderLock = true;
            AddNotification(L"Warp shader locked");
        } else if (!m_bCompShaderLock && m_bWarpShaderLock) {
            m_bCompShaderLock = true; m_bWarpShaderLock = true;
            AddNotification(L"All shaders locked");
        } else {
            m_bCompShaderLock = false; m_bWarpShaderLock = false;
            AddNotification(L"All shaders unlocked");
        }
        return true;
    case HK_SONG_TITLE:
        LaunchSongTitleAnim(-1);
        return true;
    case HK_KILL_SPRITES:
        KillAllSprites();
        return true;
    case HK_KILL_SUPERTEXTS:
        KillAllSupertexts();
        return true;
    case HK_AUTO_PRESET_CHANGE:
        m_ChangePresetWithSong = !m_ChangePresetWithSong;
        AddNotification(m_ChangePresetWithSong
            ? L"Auto Preset Change enabled" : L"Auto Preset Change disabled");
        return true;
    case HK_SCRAMBLE_WARP: {
        bool bWarpLock = m_bWarpShaderLock;
        wchar_t szOldPreset[MAX_PATH];
        lstrcpyW(szOldPreset, m_szCurrentPresetFile);
        m_bWarpShaderLock = false;
        LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bWarpShaderLock = true;
            LoadPreset(szOldPreset, 0.0f);
            WaitForPendingLoad(3000);
        }
        m_bWarpShaderLock = bWarpLock;
        return true;
    }
    case HK_SCRAMBLE_COMP: {
        bool bCompLock = m_bCompShaderLock;
        wchar_t szOldPreset[MAX_PATH];
        lstrcpyW(szOldPreset, m_szCurrentPresetFile);
        m_bCompShaderLock = false;
        LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bCompShaderLock = true;
            LoadPreset(szOldPreset, 0.0f);
            WaitForPendingLoad(3000);
        }
        m_bCompShaderLock = bCompLock;
        return true;
    }
    case HK_QUICKSAVE:
        SaveCurrentPresetToQuicksave(false);
        return true;
    case HK_SCROLL_LOCK:
        m_bPresetLockedByUser = GetKeyState(VK_SCROLL) & 1;
        TogglePlaylist();
        return true;
    case HK_RELOAD_MESSAGES:
        ReadCustomMessages();
        AddNotification(L"Messages reloaded");
        return true;

    // ── Misc ──
    case HK_DEBUG_INFO:
        m_bShowDebugInfo = !m_bShowDebugInfo;
        return true;
    case HK_SPRITE_MODE:
        if (m_nNumericInputMode == NUMERIC_INPUT_MODE_SPRITE) {
            m_nNumericInputMode = NUMERIC_INPUT_MODE_CUST_MSG;
            SendMessageToMDropDX12Remote(L"STATUS=Message Mode set");
            PostMessageToMDropDX12Remote(WM_USER_MESSAGE_MODE);
        } else {
            m_nNumericInputMode = NUMERIC_INPUT_MODE_SPRITE;
            SendMessageToMDropDX12Remote(L"STATUS=Sprite Mode set");
            PostMessageToMDropDX12Remote(WM_USER_SPRITE_MODE);
        }
        m_nNumericInputNum = 0;
        m_nNumericInputDigits = 0;
        return true;

    default:
        break;
    }

    // Dynamic user hotkeys (Script Commands and Launch Apps)
    if (actionId >= USER_HOTKEY_ID_BASE) {
        for (const auto& uh : m_userHotkeys) {
            if (uh.id == actionId) {
                if (uh.command.empty()) {
                    AddNotification(uh.type == USER_HK_SCRIPT
                        ? L"No command configured" : L"No app configured");
                    return true;
                }
                if (uh.type == USER_HK_SCRIPT) {
                    ExecuteControllerCommand(WideToUTF8(uh.command));
                } else {
                    LaunchOrFocusApp(uh.command);
                }
                return true;
            }
        }
    }

    return false;
    #undef clamp
}

// Helper: robustly bring a window to the foreground (works even when caller isn't foreground)
static void ForceForegroundWindow(HWND hwnd)
{
    if (IsIconic(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    HWND hFore = GetForegroundWindow();
    if (hFore == hwnd)
        return;

    // Attach to foreground thread to bypass SetForegroundWindow restrictions
    DWORD foreThread = GetWindowThreadProcessId(hFore, NULL);
    DWORD targetThread = GetWindowThreadProcessId(hwnd, NULL);
    if (foreThread != targetThread)
        AttachThreadInput(foreThread, targetThread, TRUE);

    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);

    if (foreThread != targetThread)
        AttachThreadInput(foreThread, targetThread, FALSE);
}

void Engine::LaunchOrFocusApp(const std::wstring& path)
{
    if (path.empty()) {
        AddNotification(L"No app configured");
        return;
    }

    // Extract exe filename from full path
    const wchar_t* exeName = wcsrchr(path.c_str(), L'\\');
    if (!exeName) exeName = wcsrchr(path.c_str(), L'/');
    exeName = exeName ? exeName + 1 : path.c_str();

    // Build title search string: exe name without extension (e.g., "MilkWave" from "MilkWave.exe")
    std::wstring titleSearch(exeName);
    size_t dot = titleSearch.rfind(L'.');
    if (dot != std::wstring::npos)
        titleSearch.erase(dot);

    // Strategy 1: Search for a running process with matching exe name
    DWORD targetPID = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                    targetPID = pe.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    // Find main window by PID
    if (targetPID != 0) {
        struct FindData { DWORD pid; HWND hwnd; } fd = { targetPID, NULL };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* d = (FindData*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == d->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
                d->hwnd = h;
                return FALSE;
            }
            return TRUE;
        }, (LPARAM)&fd);

        if (fd.hwnd) {
            ForceForegroundWindow(fd.hwnd);
            return;
        }
    }

    // Strategy 2: Window title matching — find any visible top-level window whose
    // title contains the exe name (without extension), case-insensitive.
    // Catches apps whose process name differs from the configured path.
    struct TitleFindData { const wchar_t* search; HWND hwnd; } tfd = { titleSearch.c_str(), NULL };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* d = (TitleFindData*)lp;
        if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL)
            return TRUE;
        wchar_t title[256];
        if (GetWindowTextW(h, title, 256) > 0) {
            // Case-insensitive substring search
            std::wstring t(title);
            std::wstring s(d->search);
            for (auto& c : t) c = towlower(c);
            for (auto& c : s) c = towlower(c);
            if (t.find(s) != std::wstring::npos) {
                d->hwnd = h;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&tfd);

    if (tfd.hwnd) {
        ForceForegroundWindow(tfd.hwnd);
        return;
    }

    // Not running — launch it.  Use ShellExecuteExW to get the process handle,
    // wait for it to initialize, then bring its window to the foreground.
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei) && sei.hProcess) {
        wchar_t msg[MAX_PATH + 32];
        swprintf(msg, MAX_PATH + 32, L"Launching %s", exeName);
        AddNotification(msg);

        // Fire-and-forget thread: wait for the app to initialize, then focus it.
        // Avoids blocking the message pump during WaitForInputIdle.
        HANDLE hProc = sei.hProcess;
        std::thread([hProc]() {
            WaitForInputIdle(hProc, 3000);
            DWORD newPID = GetProcessId(hProc);
            CloseHandle(hProc);

            if (newPID != 0) {
                struct FindData { DWORD pid; HWND hwnd; } fd2 = { newPID, NULL };
                EnumWindows([](HWND h, LPARAM lp) -> BOOL {
                    auto* d = (FindData*)lp;
                    DWORD pid = 0;
                    GetWindowThreadProcessId(h, &pid);
                    if (pid == d->pid && IsWindowVisible(h) && GetWindow(h, GW_OWNER) == NULL) {
                        d->hwnd = h;
                        return FALSE;
                    }
                    return TRUE;
                }, (LPARAM)&fd2);

                if (fd2.hwnd)
                    ForceForegroundWindow(fd2.hwnd);
            }
        }).detach();
    } else {
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Could not launch %s", exeName);
        AddError(msg, m_ErrorDuration, ERR_MISC, false);
    }
}

int Engine::AddUserHotkey(UserHotkeyType type)
{
    UserHotkey uh;
    uh.id = m_nextUserHotkeyId++;
    uh.type = type;
    uh.modifiers = 0;
    uh.vk = 0;
    uh.scope = (type == USER_HK_LAUNCH) ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;
    uh.label = (type == USER_HK_SCRIPT) ? L"Script Command" : L"Launch App";
    m_userHotkeys.push_back(std::move(uh));
    return (int)m_userHotkeys.size() - 1;
}

void Engine::RemoveUserHotkey(int index)
{
    if (index < 0 || index >= (int)m_userHotkeys.size()) return;
    // Unregister if global
    HWND hRender = GetPluginWindow();
    if (hRender && m_userHotkeys[index].scope == HKSCOPE_GLOBAL)
        UnregisterHotKey(hRender, m_userHotkeys[index].id);
    m_userHotkeys.erase(m_userHotkeys.begin() + index);
}

bool Engine::LookupLocalHotkey(UINT vk, UINT modifiers)
{
    // Check local bindings (vk/modifiers fields are always the local binding)
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].vk == vk && m_hotkeys[i].vk != 0 &&
            m_hotkeys[i].modifiers == modifiers)
        {
            return DispatchHotkeyAction(m_hotkeys[i].id);
        }
    }
    // Also check dynamic user hotkeys
    for (const auto& uh : m_userHotkeys) {
        if (uh.vk == vk && uh.vk != 0 &&
            uh.modifiers == modifiers &&
            uh.scope == HKSCOPE_LOCAL)
        {
            return DispatchHotkeyAction(uh.id);
        }
    }
    return false;
}

bool Engine::DispatchHotkeyByTag(const std::wstring& tag)
{
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (_wcsicmp(m_hotkeys[i].szIniKey, tag.c_str()) == 0)
            return DispatchHotkeyAction(m_hotkeys[i].id);
    }
    return false;
}

void Engine::LoadIdleTimerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    m_bIdleTimerEnabled = GetPrivateProfileIntW(L"IdleTimer", L"Enabled", 0, pIni) != 0;
    m_nIdleTimeoutMinutes = GetPrivateProfileIntW(L"IdleTimer", L"TimeoutMinutes", 5, pIni);
    if (m_nIdleTimeoutMinutes < 1) m_nIdleTimeoutMinutes = 1;
    if (m_nIdleTimeoutMinutes > 60) m_nIdleTimeoutMinutes = 60;
    m_nIdleAction = GetPrivateProfileIntW(L"IdleTimer", L"Action", 0, pIni);
    if (m_nIdleAction < 0 || m_nIdleAction > 1) m_nIdleAction = 0;
    m_bIdleAutoRestore = GetPrivateProfileIntW(L"IdleTimer", L"AutoRestore", 1, pIni) != 0;
}

void Engine::SaveIdleTimerSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    WritePrivateProfileStringW(L"IdleTimer", L"Enabled", m_bIdleTimerEnabled ? L"1" : L"0", pIni);
    swprintf(buf, 64, L"%d", m_nIdleTimeoutMinutes);
    WritePrivateProfileStringW(L"IdleTimer", L"TimeoutMinutes", buf, pIni);
    swprintf(buf, 64, L"%d", m_nIdleAction);
    WritePrivateProfileStringW(L"IdleTimer", L"Action", buf, pIni);
    WritePrivateProfileStringW(L"IdleTimer", L"AutoRestore", m_bIdleAutoRestore ? L"1" : L"0", pIni);
}

std::wstring Engine::FormatHotkeyDisplay(UINT modifiers, UINT vk)
{
    if (vk == 0) return L"(none)";

    std::wstring result;
    if (modifiers & MOD_CONTROL) result += L"CTRL+";
    if (modifiers & MOD_ALT)     result += L"ALT+";
    if (modifiers & MOD_SHIFT)   result += L"SHIFT+";
    if (modifiers & MOD_WIN)     result += L"WIN+";

    // Map virtual key to name
    switch (vk) {
    case VK_LBUTTON:    result += L"Left Mouse"; break;
    case VK_RBUTTON:    result += L"Right Mouse"; break;
    case VK_MBUTTON:    result += L"Middle Mouse"; break;
    case VK_XBUTTON1:   result += L"X1 Mouse"; break;
    case VK_XBUTTON2:   result += L"X2 Mouse"; break;
    case VK_RETURN:     result += L"ENTER"; break;
    case VK_ESCAPE:     result += L"ESC"; break;
    case VK_SPACE:      result += L"SPACE"; break;
    case VK_TAB:        result += L"TAB"; break;
    case VK_BACK:       result += L"BACKSPACE"; break;
    case VK_DELETE:     result += L"DELETE"; break;
    case VK_INSERT:     result += L"INSERT"; break;
    case VK_HOME:       result += L"HOME"; break;
    case VK_END:        result += L"END"; break;
    case VK_PRIOR:      result += L"PGUP"; break;
    case VK_NEXT:       result += L"PGDN"; break;
    case VK_UP:         result += L"UP"; break;
    case VK_DOWN:       result += L"DOWN"; break;
    case VK_LEFT:       result += L"LEFT"; break;
    case VK_RIGHT:      result += L"RIGHT"; break;
    case VK_SCROLL:     result += L"SCROLL LOCK"; break;
    case VK_OEM_3:      result += L"`"; break;
    case VK_OEM_4:      result += L"["; break;
    case VK_OEM_6:      result += L"]"; break;
    case VK_OEM_COMMA:  result += L","; break;
    case VK_OEM_PERIOD: result += L"."; break;
    case VK_OEM_MINUS:  result += L"-"; break;
    case VK_OEM_PLUS:   result += L"="; break;
    case VK_OEM_1:      result += L";"; break;
    case VK_OEM_2:      result += L"/"; break;
    case VK_OEM_5:      result += L"\\"; break;
    case VK_OEM_7:      result += L"'"; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F24) {
            wchar_t fbuf[8];
            swprintf(fbuf, 8, L"F%d", vk - VK_F1 + 1);
            result += fbuf;
        } else if (vk >= 'A' && vk <= 'Z') {
            result += (wchar_t)vk;
        } else if (vk >= '0' && vk <= '9') {
            result += (wchar_t)vk;
        } else {
            wchar_t kbuf[16];
            swprintf(kbuf, 16, L"0x%02X", vk);
            result += kbuf;
        }
        break;
    }
    return result;
}

// ── Dynamic F1 Help Text ──

void Engine::GenerateHelpText()
{
    // Build all help text into a single buffer. Pagination happens at render
    // time based on window height so pages adapt to any resolution.
    // Order: fixed keys first, then reassignable hotkeys grouped by category,
    // sorted alphabetically by action within each group.

    wchar_t* p = m_szHelpAll;
    int rem = _countof(m_szHelpAll) - 1;
    int lineCount = 0;

    auto appendLine = [&](const wchar_t* line) {
        int len = (int)wcslen(line);
        if (rem > len + 2) {
            wcscpy(p, line);
            p += len;
            *p++ = L'\n'; rem -= len + 1;
            lineCount++;
        }
    };

    // ── Fixed keys first ──
    appendLine(L"\x2500\x2500\x2500 Fixed Keys (not reassignable) \x2500\x2500\x2500");
    appendLine(L"  F1                             Toggle Help Overlay");
    appendLine(L"  F2                             (reserved)");
    appendLine(L"  CTRL+F2                        Kill Switch \x2014 disable all outputs");
    appendLine(L"  ESC                            Close menu / Close app");
    appendLine(L"  0-9                            Numeric input (sprites/messages)");
    appendLine(L"");

    // ── Reassignable hotkeys, grouped by category, sorted by action ──
    struct HelpEntry { std::wstring key; std::wstring action; };

    for (int ci = 0; ci < HKCAT_COUNT; ci++) {
        int cat = m_helpCatOrder[ci];
        std::vector<HelpEntry> entries;

        // Collect bound built-in hotkeys in this category
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            if ((int)m_hotkeys[i].category != cat) continue;
            if (m_hotkeys[i].vk == 0 && m_hotkeys[i].globalVK == 0) continue;
            HelpEntry e;
            if (m_hotkeys[i].vk != 0 && m_hotkeys[i].globalVK != 0) {
                e.key = FormatHotkeyDisplay(m_hotkeys[i].modifiers, m_hotkeys[i].vk);
                e.key += L" / ";
                e.key += FormatHotkeyDisplay(m_hotkeys[i].globalMod, m_hotkeys[i].globalVK);
            } else if (m_hotkeys[i].vk != 0) {
                e.key = FormatHotkeyDisplay(m_hotkeys[i].modifiers, m_hotkeys[i].vk);
            } else {
                e.key = FormatHotkeyDisplay(m_hotkeys[i].globalMod, m_hotkeys[i].globalVK);
                e.key += L" (G)";
            }
            e.action = m_hotkeys[i].szAction;
            entries.push_back(std::move(e));
        }

        // Collect user hotkeys under Script/Launch categories
        if (cat == HKCAT_SCRIPT || cat == HKCAT_LAUNCH) {
            UserHotkeyType matchType = (cat == HKCAT_SCRIPT) ? USER_HK_SCRIPT : USER_HK_LAUNCH;
            for (const auto& uh : m_userHotkeys) {
                if (uh.type != matchType || uh.vk == 0) continue;
                HelpEntry e;
                e.key = FormatHotkeyDisplay(uh.modifiers, uh.vk);
                e.action = uh.label;
                if (!uh.command.empty()) {
                    if (uh.type == USER_HK_SCRIPT) {
                        e.action += L" (" + uh.command + L")";
                    } else {
                        const wchar_t* exeName = wcsrchr(uh.command.c_str(), L'\\');
                        if (!exeName) exeName = wcsrchr(uh.command.c_str(), L'/');
                        exeName = exeName ? exeName + 1 : uh.command.c_str();
                        e.action += L" (";
                        e.action += exeName;
                        e.action += L")";
                    }
                }
                entries.push_back(std::move(e));
            }
        }

        if (entries.empty()) continue;

        // Sort alphabetically by action (case-insensitive)
        std::sort(entries.begin(), entries.end(), [](const HelpEntry& a, const HelpEntry& b) {
            return _wcsicmp(a.action.c_str(), b.action.c_str()) < 0;
        });

        // Category header
        wchar_t header[128];
        swprintf(header, 128, L"\x2500\x2500\x2500 %s \x2500\x2500\x2500", kCategoryNames[cat]);
        appendLine(header);

        for (const auto& e : entries) {
            wchar_t line[256];
            swprintf(line, 256, L"  %-32s %s", e.key.c_str(), e.action.c_str());
            appendLine(line);
        }
        appendLine(L"");
    }

    // Look up actual binding for "Open Hotkeys"
    std::wstring hotkeyHint;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].id == HK_OPEN_HOTKEYS && m_hotkeys[i].vk != 0) {
            hotkeyHint = FormatHotkeyDisplay(m_hotkeys[i].modifiers, m_hotkeys[i].vk);
            break;
        }
    }
    wchar_t footer[256];
    if (!hotkeyHint.empty())
        swprintf(footer, 256, L"Reassign keys in the Hotkeys window (%s)", hotkeyHint.c_str());
    else
        swprintf(footer, 256, L"Reassign keys in the Hotkeys window");
    appendLine(footer);

    *p = L'\0';
    m_nHelpLineCount = lineCount;
}

} // namespace mdrop
