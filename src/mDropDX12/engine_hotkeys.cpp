// engine_hotkeys.cpp — Configurable hotkey load/save/register/dispatch
//
// Part of the MDropDX12 configurable hotkeys system.
// Supports local (render-window-focus) and global (system-wide) bindings.
// ~84 reassignable bindings; F1/F2/Ctrl+F2/Escape remain hardcoded.

#include "engine.h"
#include "json_utils.h"
#include "mixer_sonar_restart.h"
#include "shader_overrides.h"
#include "format_to.h"
#include "tool_window.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "wasabi.h"
#include <TlHelp32.h>
#include <thread>
#include "config_store.h"
#include "pipe_server.h"

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
    HK_DEF(i++, HK_SHOW_BATTERY,      0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Show Battery",          L"ShowBattery");
    HK_DEF(i++, HK_SHOW_RATING,       0,                     VK_F6,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Show Rating",           L"ShowRating");
    HK_DEF(i++, HK_SHOW_SHADER_HELP,  0,                     VK_F9,        HKSCOPE_LOCAL, HKCAT_WINDOW, L"Shader Help",           L"ShowShaderHelp");
    HK_DEF(i++, HK_WATERMARK,        0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Watermark",             L"Watermark");
    HK_DEF(i++, HK_MIRROR_WATERMARK, 0,                     0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Mirror Watermark",      L"MirrorWatermark");
    HK_DEF(i++, HK_MIRROR_INDEPENDENT, 0,                   0,            HKSCOPE_LOCAL, HKCAT_WINDOW, L"Mirror Independent Render", L"MirrorIndependent");

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
    HK_DEF(i++, HK_OPEN_CUSTOM_SHADERS,0,                  0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Custom Shaders",  L"OpenCustomShaders");
    HK_DEF(i++, HK_TOGGLE_SHADER_OVERRIDES,0,              0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Toggle Shader Overrides", L"ToggleShaderOverrides");
    HK_DEF(i++, HK_OPEN_WORKSPACE_LAYOUT,0,               0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Workspace Layout", L"OpenWorkspaceLayout");
    HK_DEF(i++, HK_APPLY_WORKSPACE_LAYOUT,0,              0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Apply Workspace Layout", L"ApplyWorkspaceLayout");
    HK_DEF(i++, HK_OPEN_TEXT_ANIM,   0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Text Animations",  L"OpenTextAnim");
    HK_DEF(i++, HK_OPEN_REMOTE,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Remote",           L"OpenRemote");
    HK_DEF(i++, HK_OPEN_VISUAL,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Visual",           L"OpenVisual");
    HK_DEF(i++, HK_OPEN_COLORS,     0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Colors",           L"OpenColors");
    HK_DEF(i++, HK_OPEN_CONTROLLER, 0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Controller",       L"OpenController");
    HK_DEF(i++, HK_OPEN_ANNOTATIONS,0,                    0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Annotations",      L"OpenAnnotations");
    HK_DEF(i++, HK_OPEN_SCRIPT,    0,                     0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Script",           L"OpenScript");
    HK_DEF(i++, HK_OPEN_PRESET_EDITOR, 0,                 0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Preset Editor",    L"OpenPresetEditor");

    // Audio mixer. Every one is unbound: the user picks the keys, and a volume
    // key that arrived pre-bound would fight whatever it collided with.
    HK_DEF(i++, HK_MIXER_GROUP1_UP,     0,                 0,            HKSCOPE_LOCAL, HKCAT_MEDIA, L"Mixer: Group 1 Volume Up",     L"MixerGroup1Up");
    HK_DEF(i++, HK_MIXER_GROUP1_DOWN,   0,                 0,            HKSCOPE_LOCAL, HKCAT_MEDIA, L"Mixer: Group 1 Volume Down",   L"MixerGroup1Down");
    HK_DEF(i++, HK_MIXER_GROUP1_MUTE,   0,                 0,            HKSCOPE_LOCAL, HKCAT_MEDIA, L"Mixer: Group 1 Mute",          L"MixerGroup1Mute");
    HK_DEF(i++, HK_MIXER_SONAR_RESTART, 0,                 0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Mixer: Restart SteelSeries",   L"MixerSonarRestart");
    HK_DEF(i++, HK_OPEN_AUDIO_MIXER,    0,                 0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Open Audio Mixer",             L"OpenAudioMixer");
    HK_DEF(i++, HK_SAVE_DISPLAY_SNAPSHOT, 0,                0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Save Display Snapshot",        L"SaveDisplaySnapshot");
    HK_DEF(i++, HK_LOAD_DEFAULT_PROFILE,  0,                0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Load Default Display Profile", L"LoadDefaultDisplayProfile");
    HK_DEF(i++, HK_NEXT_DISPLAY_PROFILE,  0,                0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Next Display Profile",         L"NextDisplayProfile");
    HK_DEF(i++, HK_PREV_DISPLAY_PROFILE,  0,                0,            HKSCOPE_LOCAL, HKCAT_TOOLS, L"Previous Display Profile",     L"PrevDisplayProfile");
    HK_DEF(i++, HK_POLL_TRACK_INFO, 0,                     0,            HKSCOPE_LOCAL, HKCAT_MEDIA, L"Poll Track Info",       L"PollTrackInfo");

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
    HK_DEF(i++, HK_TOGGLE_MESSAGES,   0,                     0,            HKSCOPE_LOCAL, HKCAT_MISC, L"Toggle Messages",        L"ToggleMessages");
    HK_DEF(i++, HK_SETTINGS_OVERLAY,  0,                     0,            HKSCOPE_LOCAL, HKCAT_MISC, L"Settings Overlay",       L"SettingsOverlay");

    // User-added hotkeys (Script/Launch) are NOT reset here — they're user-created.
}

#undef HK_DEF
#undef HK_DEF2

// ── Hotkeys on disk: resources/hotkeys.json ─────────────────────────────
//
// This was 607 keys in settings.ini -- 53% of the whole file, one section --
// and 76% of those values were zero, because every action writes five keys
// whether or not anybody bound it. The goal is for settings.ini to get smaller
// over time, and no single change takes more out of it than this one.
//
// ONLY WHAT DIFFERS FROM THE DEFAULT IS WRITTEN. An action nobody has touched
// appears nowhere; a locally-bound action carries no global fields. That is
// what turns 607 keys into a file a person can read, and it is also why the
// loader starts from ResetHotkeyDefaults() and applies the file on top: absent
// means "default", not "zero".
//
// The name is the identity, not the position. szIniKey keyed the INI and keys
// the json, so reordering the hotkey table -- or inserting an action into the
// middle of it -- cannot silently re-point somebody's bindings.

std::wstring Engine::HotkeyStorePath() const
{
    std::wstring p = m_szMilkdrop2Path;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    return p + L"hotkeys.json";
}

void Engine::SaveHotkeysJson()
{
    JsonWriter w;
    w.BeginObject();
    w.Int(L"version", 1);

    w.BeginObject(L"bindings");
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        const auto& h = m_hotkeys[i];
        // Nothing to say about an action still at its default.
        // Against the action's OWN defaults, not against zero: several ship
        // bound (OpenSettings is F8), and treating zero as the default would
        // write those out forever while silently losing a deliberate unbind.
        const bool differs = h.modifiers != h.defaultMod || h.vk != h.defaultVK ||
                             h.scope != h.defaultScope ||
                             h.globalMod != h.defaultGlobalMod ||
                             h.globalVK != h.defaultGlobalVK;
        if (!differs) continue;

        w.BeginObject(h.szIniKey);
        if (h.modifiers != h.defaultMod)          w.Int(L"mod", (int)h.modifiers);
        if (h.vk != h.defaultVK)                  w.Int(L"vk", (int)h.vk);
        if (h.scope != h.defaultScope)            w.Int(L"scope", (int)h.scope);
        if (h.globalMod != h.defaultGlobalMod)    w.Int(L"globalMod", (int)h.globalMod);
        if (h.globalVK != h.defaultGlobalVK)      w.Int(L"globalVK", (int)h.globalVK);
        w.EndObject();
    }
    w.EndObject();

    // The dynamic Script/Launch entries. An array, because their ORDER is the
    // order the Hotkeys window lists them in -- the INI encoded that in the key
    // name (UserHotkey_<n>_) and then needed a separate pass to sweep up stale
    // entries when the list got shorter. An array cannot go stale.
    w.Int(L"nextUserId", m_nextUserHotkeyId);

    // Built as a JsonValue rather than written inline: JsonWriter can open a
    // named object and an anonymous array, but not an anonymous object inside
    // an array, which is exactly what a list of records needs. mixer_settings
    // builds its allowlist the same way for the same reason.
    JsonValue users;
    users.type = JsonValue::Array;
    for (const auto& uh : m_userHotkeys) {
        JsonValue e;
        e.type = JsonValue::Object;
        e.members.push_back({ L"id",   JsonValue(uh.id) });
        e.members.push_back({ L"type", JsonValue((int)uh.type) });
        if (uh.modifiers)        e.members.push_back({ L"mod",   JsonValue((int)uh.modifiers) });
        if (uh.vk)               e.members.push_back({ L"vk",    JsonValue((int)uh.vk) });
        if (uh.scope)            e.members.push_back({ L"scope", JsonValue((int)uh.scope) });
        if (!uh.label.empty())   e.members.push_back({ L"label", JsonValue(uh.label) });
        if (!uh.command.empty()) e.members.push_back({ L"command", JsonValue(uh.command) });
        users.elements.push_back(e);
    }
    w.Value(L"userHotkeys", users);
    w.EndObject();

    const std::wstring path = HotkeyStorePath();
    if (!w.SaveToFile(path.c_str()))
        DLOG_ERROR("hotkeys: could not write %ls", path.c_str());
}

bool Engine::LoadHotkeysJson()
{
    const std::wstring path = HotkeyStorePath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    JsonValue root;
    try {
        root = JsonLoadFile(path.c_str());
    } catch (...) {
        DLOG_ERROR("hotkeys: %ls is malformed; falling back to defaults", path.c_str());
        return false;
    }
    if (!root.isObject()) return false;

    // The caller has already applied ResetHotkeyDefaults(), so anything the
    // file does not mention keeps the default it was born with.
    const JsonValue& b = root[L"bindings"];
    if (b.isObject()) {
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            const JsonValue& e = b[m_hotkeys[i].szIniKey];
            if (!e.isObject()) continue;
            m_hotkeys[i].modifiers = (UINT)e[L"mod"].asInt((int)m_hotkeys[i].defaultMod);
            m_hotkeys[i].vk        = (UINT)e[L"vk"].asInt((int)m_hotkeys[i].defaultVK);
            m_hotkeys[i].scope     = (HotkeyScope)e[L"scope"].asInt((int)m_hotkeys[i].defaultScope);
            m_hotkeys[i].globalMod = (UINT)e[L"globalMod"].asInt((int)m_hotkeys[i].defaultGlobalMod);
            m_hotkeys[i].globalVK  = (UINT)e[L"globalVK"].asInt((int)m_hotkeys[i].defaultGlobalVK);
        }
    }

    m_userHotkeys.clear();
    m_nextUserHotkeyId = root[L"nextUserId"].asInt(USER_HOTKEY_ID_BASE);
    const JsonValue& arr = root[L"userHotkeys"];
    for (size_t i = 0; i < arr.size(); i++) {
        const JsonValue& e = arr.at(i);
        UserHotkey uh;
        uh.id        = e[L"id"].asInt(USER_HOTKEY_ID_BASE + (int)i);
        uh.type      = (UserHotkeyType)e[L"type"].asInt(0);
        uh.modifiers = (UINT)e[L"mod"].asInt(0);
        uh.vk        = (UINT)e[L"vk"].asInt(0);
        uh.scope     = (HotkeyScope)e[L"scope"].asInt(0);
        uh.label     = e[L"label"].asString();
        uh.command   = e[L"command"].asString();
        m_userHotkeys.push_back(std::move(uh));
    }
    return true;
}

// Called once, after the INI reader above has populated the table from a
// settings.ini that still had a [Hotkeys] section.
//
// Writes the json, READS IT BACK and compares every binding against what is in
// memory, and only then removes the section. A mismatch keeps the section
// exactly where it is -- settings.ini is no more disposable than the profile
// stores were, and this is the same shape of migration they use.
void Engine::MigrateHotkeysToJson()
{
    SaveHotkeysJson();

    std::vector<HotkeyBinding> want(m_hotkeys, m_hotkeys + NUM_HOTKEYS);
    const std::vector<UserHotkey> wantUser = m_userHotkeys;
    const int wantNext = m_nextUserHotkeyId;

    if (!LoadHotkeysJson()) {
        DLOG_ERROR("hotkeys: migration wrote a file it could not read back; "
                   "keeping [Hotkeys] in settings.ini");
        return;
    }
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].modifiers == want[i].modifiers &&
            m_hotkeys[i].vk == want[i].vk &&
            m_hotkeys[i].scope == want[i].scope &&
            m_hotkeys[i].globalMod == want[i].globalMod &&
            m_hotkeys[i].globalVK == want[i].globalVK)
            continue;
        DLOG_ERROR("hotkeys: '%ls' did not survive the migration; "
                   "keeping [Hotkeys] in settings.ini", m_hotkeys[i].szIniKey);
        return;
    }
    if (m_userHotkeys.size() != wantUser.size() || m_nextUserHotkeyId != wantNext) {
        DLOG_ERROR("hotkeys: %zu of %zu user hotkeys survived; "
                   "keeping [Hotkeys] in settings.ini",
                   m_userHotkeys.size(), wantUser.size());
        return;
    }

    Config().RemoveSection(L"Hotkeys");
    ConfigFlushAll();      // the point is that settings.ini is smaller NOW
    DLOG_INFO("hotkeys: moved to %ls; [Hotkeys] removed from settings.ini",
              HotkeyStorePath().c_str());
}


void Engine::LoadHotkeySettings()
{

    // Start from defaults
    ResetHotkeyDefaults();

    // resources/hotkeys.json is where bindings live now. The whole INI reader
    // below it exists only to migrate a settings.ini that still has a
    // [Hotkeys] section, and runs once.
    if (LoadHotkeysJson()) {
        DLOG_INFO("hotkeys: loaded from %ls", HotkeyStorePath().c_str());
        return;
    }

    // Version marker:
    // 0 (absent) = pre-expansion (only 10 hotkeys)
    // 2 = full reassignable hotkeys (fixed Script/Launch slots)
    // 3 = dynamic user hotkeys (Script/Launch replaced by vector)
    // 4 = dual local/global bindings per action
    static constexpr int HOTKEY_INI_VERSION = 4;
    int iniVersion = Config().GetInt(L"Hotkeys", L"Version", 0);

    // Migration: if old "Enabled" key exists, migrate scope for configured bindings
    int oldEnabled = Config().GetInt(L"Hotkeys", L"Enabled", -1);

    if (iniVersion >= 2) {
        // Read built-in hotkey overrides from INI
        for (int i = 0; i < NUM_HOTKEYS; i++) {
            wchar_t modKey[128], vkKey[128], scopeKey[128];
            swprintf(modKey, 128, L"%s_Mod", m_hotkeys[i].szIniKey);
            swprintf(vkKey, 128, L"%s_VK", m_hotkeys[i].szIniKey);
            swprintf(scopeKey, 128, L"%s_Scope", m_hotkeys[i].szIniKey);

            m_hotkeys[i].modifiers = (UINT)Config().GetInt(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers);
            m_hotkeys[i].vk = (UINT)Config().GetInt(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk);
            m_hotkeys[i].scope = (HotkeyScope)Config().GetInt(L"Hotkeys", scopeKey, (int)m_hotkeys[i].scope);

            if (iniVersion >= 4) {
                // Version 4+: read global binding
                wchar_t gModKey[128], gVkKey[128];
                swprintf(gModKey, 128, L"%s_GlobalMod", m_hotkeys[i].szIniKey);
                swprintf(gVkKey, 128, L"%s_GlobalVK", m_hotkeys[i].szIniKey);
                m_hotkeys[i].globalMod = (UINT)Config().GetInt(L"Hotkeys", gModKey, (int)m_hotkeys[i].globalMod);
                m_hotkeys[i].globalVK = (UINT)Config().GetInt(L"Hotkeys", gVkKey, (int)m_hotkeys[i].globalVK);
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

            m_hotkeys[i].modifiers = (UINT)Config().GetInt(L"Hotkeys", modKey, (int)m_hotkeys[i].modifiers);
            m_hotkeys[i].vk = (UINT)Config().GetInt(L"Hotkeys", vkKey, (int)m_hotkeys[i].vk);
            m_hotkeys[i].scope = (HotkeyScope)Config().GetInt(L"Hotkeys", scopeKey, (int)m_hotkeys[i].scope);
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
        Config().Remove(L"Hotkeys", L"Enabled");
    }

    // Version 2→3 cleanup: remove old fixed Script/Launch keys
    if (iniVersion == 2) {
        for (int i = 1; i <= 10; i++) {
            wchar_t key[64];
            swprintf(key, 64, L"Script%d_Cmd", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"Script%d_Mod", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"Script%d_VK", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"Script%d_Scope", i);
            Config().Remove(L"Hotkeys", key);
        }
        for (int i = 1; i <= 4; i++) {
            wchar_t key[64];
            swprintf(key, 64, L"LaunchApp%d_Path", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"LaunchApp%d_Mod", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"LaunchApp%d_VK", i);
            Config().Remove(L"Hotkeys", key);
            swprintf(key, 64, L"LaunchApp%d_Scope", i);
            Config().Remove(L"Hotkeys", key);
        }
    }

    // Load dynamic user hotkeys (Version 3+)
    m_userHotkeys.clear();
    if (iniVersion >= 3) {
        int count = Config().GetInt(L"Hotkeys", L"UserHotkey_Count", 0);
        m_nextUserHotkeyId = Config().GetInt(L"Hotkeys", L"UserHotkey_NextID", USER_HOTKEY_ID_BASE);

        for (int i = 0; i < count; i++) {
            wchar_t prefix[64];
            swprintf(prefix, 64, L"UserHotkey_%d_", i);

            UserHotkey uh;
            wchar_t keyBuf[128], valBuf[512];

            swprintf(keyBuf, 128, L"%sID", prefix);
            uh.id = Config().GetInt(L"Hotkeys", keyBuf, USER_HOTKEY_ID_BASE + i);

            swprintf(keyBuf, 128, L"%sType", prefix);
            uh.type = (UserHotkeyType)Config().GetInt(L"Hotkeys", keyBuf, 0);

            swprintf(keyBuf, 128, L"%sMod", prefix);
            uh.modifiers = (UINT)Config().GetInt(L"Hotkeys", keyBuf, 0);

            swprintf(keyBuf, 128, L"%sVK", prefix);
            uh.vk = (UINT)Config().GetInt(L"Hotkeys", keyBuf, 0);

            swprintf(keyBuf, 128, L"%sScope", prefix);
            uh.scope = (HotkeyScope)Config().GetInt(L"Hotkeys", keyBuf, 0);

            swprintf(keyBuf, 128, L"%sLabel", prefix);
            Config().GetStringTo(L"Hotkeys", keyBuf, L"", valBuf, 512);
            uh.label = valBuf;

            swprintf(keyBuf, 128, L"%sCmd", prefix);
            Config().GetStringTo(L"Hotkeys", keyBuf, L"", valBuf, 512);
            uh.command = valBuf;

            m_userHotkeys.push_back(std::move(uh));
        }
    }

    // Everything above read a [Hotkeys] section out of settings.ini. Write it
    // to json, verify it, and take the section out -- 607 keys, 53% of that
    // file. iniVersion is no longer consulted for anything after this: the
    // section it described will not be there next time.
    (void)HOTKEY_INI_VERSION;
    MigrateHotkeysToJson();
}

void Engine::SaveHotkeySettings()
{
    // One line, and deliberately so. This used to write 607 keys into
    // settings.ini and then sweep up stale UserHotkey_<n>_ entries by hand,
    // because the INI encoded list position in the key name. The json holds the
    // list as an array, which cannot go stale, and omits every field still at
    // its default.
    SaveHotkeysJson();
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
    wchar_t buf[256] = {};
    Config().GetStringTo(L"Hotkeys", L"HelpCatOrder", L"", buf, 256);
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
    Config().SetString(L"Hotkeys", L"HelpCatOrder", buf);
}

// Is another full instance of this same executable running?
//
// "In use by another application" is true but unhelpful when the other
// application is MDropDX12 itself, and that is the collision a user actually
// meets: his visualizer is already running and holding every global binding,
// so a second copy -- a fresh build, a harness instance, a double-clicked
// preset that started one -- finds all of them taken and blames a mystery.
//
// The IMAGE PATH is compared rather than the process name, the same test
// PipeServer's discovery makes: two installs of the same exe are different
// applications, and naming the wrong one is worse than saying nothing.
//
// Children are excluded by asking for the shared "Milkwave_" pipe, which only
// a full instance serves -- a child serves mDxChild_ instead. That matters
// here: a child registers no global hotkey at all, so it can never be the
// cause, and counting one would put a wrong explanation in front of the user.
// WaitNamedPipe with a zero timeout asks whether the pipe exists without
// connecting to it or disturbing whoever is listening.
static bool AnotherFullInstanceIsRunning()
{
    wchar_t myPath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, myPath, MAX_PATH)) return false;
    const DWORD myPid = GetCurrentProcessId();

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (pe.th32ProcessID == myPid) continue;

            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                       pe.th32ProcessID);
            if (!hProc) continue;
            wchar_t theirPath[MAX_PATH] = {};
            DWORD len = MAX_PATH;
            const BOOL got = QueryFullProcessImageNameW(hProc, 0, theirPath, &len);
            CloseHandle(hProc);
            if (!got || _wcsicmp(theirPath, myPath) != 0) continue;

            wchar_t pipeName[64];
            FormatTo(pipeName, L"\\\\.\\pipe\\Milkwave_%u", pe.th32ProcessID);
            if (WaitNamedPipeW(pipeName, 0)) { found = true; break; }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return found;
}

// Which of the three things a registration pass does in this process.
//
// The whole point of routing both the pass and the diagnostics through one
// function is that they cannot disagree. Do not re-test m_bChildMode or
// IsTestInstance() at a call site; ask here.
GlobalHotkeyMode Engine::GlobalHotkeyRegistrationMode() const
{
    if (m_bChildMode)    return GHKMODE_NONE;
    if (IsTestInstance()) return GHKMODE_PROBE;
    return GHKMODE_REGISTER;
}

void Engine::RegisterGlobalHotkeys(HWND hwnd)
{
    if (!hwnd) return;

    // A -child instance registers nothing system-wide and does not ask
    // either. RegisterHotKey is first-come-first-served per key combination,
    // so a child would either fail (the parent already holds the key) or, if
    // it started first, steal every global binding and act on it while the
    // parent stayed silent. Children are driven over the named pipe and take
    // no direct input.
    const GlobalHotkeyMode mode = GlobalHotkeyRegistrationMode();
    if (mode == GHKMODE_NONE) return;

    // A test instance ASKS but does not keep -- see GHKMODE_PROBE. Local
    // (window-focused) hotkeys are untouched either way: they only fire when
    // the test window has focus, which is already something a test avoids.
    if (mode == GHKMODE_PROBE)
        DLOG_INFO("Global hotkeys probed, not held: test instance");

    // RegisterHotKey requires the calling thread to own the window, and
    // answers ERROR_WINDOW_OF_OTHER_THREAD (1408) for every binding when it
    // does not. That failure mode is worse than useless: the state table would
    // record all nine bindings as lost to "another application" when nothing
    // of the sort happened, and the overlay would say so.
    //
    // Both real callers are on the window thread -- startup in
    // CreateWindowAndRun, and the WM_MW_REGISTER_HOTKEYS handler that
    // SaveAndReRegister posts to. The one that was not is why this guard
    // exists: LaunchMessage runs on the RENDER thread (WM_MW_IPC_MESSAGE
    // enqueues RenderCmd::IPCMessage), so an IPC handler calling this directly
    // silently marked every global hotkey as stolen. Refuse and say so rather
    // than write a table of lies; the caller must marshal to the window
    // thread.
    if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
        DLOG_ERROR("RegisterGlobalHotkeys called from thread %lu but the window "
                   "belongs to %lu -- refusing; post WM_MW_REGISTER_HOTKEYS instead",
                   GetCurrentThreadId(), GetWindowThreadProcessId(hwnd, nullptr));
        return;
    }

    // Every return is checked. RegisterHotKey answers FALSE with
    // ERROR_HOTKEY_ALREADY_REGISTERED when another process holds the
    // combination, which is precisely the fact the user needs and precisely
    // what used to be discarded here (forgejo#72).
    //
    // A failure never aborts the pass. The bindings are independent, and
    // abandoning the rest because one was taken would turn one dead key into
    // all of them.
    std::vector<GlobalHotkeyState> fresh;
    auto claim = [&](int id, UINT mod, UINT vk) {
        GlobalHotkeyState st{ id, mod, vk, false, 0, mode == GHKMODE_PROBE };
        SetLastError(0);
        st.held = RegisterHotKey(hwnd, id, mod | MOD_NOREPEAT, vk) != FALSE;
        if (!st.held) st.lastError = GetLastError();
        // Released IMMEDIATELY on a probe, in the same statement that took it,
        // exactly as the availability scan does. Anything later -- the end of
        // the loop, the end of the function -- is a window in which an early
        // return leaves a harness copy holding one of the user's keys for the
        // rest of the session.
        //
        // st.held keeps what Windows ANSWERED. That is the fact worth
        // recording: it is what a real registration would have got, which is
        // the only thing a collision test can be asking about.
        if (st.held && st.probed) UnregisterHotKey(hwnd, id);
        fresh.push_back(st);
    };

    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].globalVK != 0)
            claim(m_hotkeys[i].id, m_hotkeys[i].globalMod, m_hotkeys[i].globalVK);
    }
    for (const auto& uh : m_userHotkeys) {
        if (uh.vk != 0 && uh.scope == HKSCOPE_GLOBAL)
            claim(uh.id, uh.modifiers, uh.vk);
    }

    // Which combinations are newly lost. SaveAndReRegister posts a whole
    // re-registration after EVERY edit in the Hotkeys window, so announcing
    // the failures unconditionally would re-announce the same ones on every
    // OK press until they read as noise. Only a change is news.
    std::vector<std::wstring> lost;
    const size_t nAsked = fresh.size();   // before the swap below empties it
    {
        std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
        auto wasHeld = [&](UINT mod, UINT vk) {
            for (const auto& p : m_globalHotkeyState)
                if (p.modifiers == mod && p.vk == vk) return p.held;
            return true;   // unknown last time: not a change, so not news
        };
        for (const auto& st : fresh)
            if (!st.held && wasHeld(st.modifiers, st.vk))
                lost.push_back(FormatHotkeyDisplay(st.modifiers, st.vk));
        m_globalHotkeyState.swap(fresh);
    }

    if (lost.empty()) return;

    // A probe instance says nothing on screen. It is not going to use these
    // keys, so "9 global hotkeys are in use by another application" is a
    // notification nobody can act on -- and it fires on every harness launch,
    // because the user's own copy is running and holding all of them. The
    // finding is in the state table for DIAG_HOTKEYS to report; that is where
    // a test looks for it.
    if (mode == GHKMODE_PROBE) {
        DLOG_INFO("Global hotkey probe: %d of %d combination(s) already taken",
                  (int)lost.size(), (int)nAsked);
        return;
    }

    // ERROR_HOTKEY_ALREADY_REGISTERED gets its own words. "in use by another
    // application" is something a user can act on; a bare failure code is not.
    // Windows does not say WHICH application and there is no way to ask, but
    // the user generally knows -- saying "not ours" is the whole job.
    bool anyForeign = false;
    {
        std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
        for (const auto& st : m_globalHotkeyState)
            if (!st.held && st.lastError == ERROR_HOTKEY_ALREADY_REGISTERED)
                anyForeign = true;
    }

    std::wstring combos;
    for (size_t i = 0; i < lost.size(); i++) {
        if (i) combos += L", ";
        combos += lost[i];
    }
    // Naming our own copy when it is the likely holder. Checked only when a
    // combination was actually lost to somebody, so the cost is paid on the
    // failure path and never on a clean start.
    //
    // Phrased as "may be holding them", not as the cause. Windows does not say
    // who owns a combination and there is no way to ask, so "another copy is
    // running" and "that copy took this key" are two different facts and only
    // the first one is known. Asserting the second would be the same shape of
    // error as the held flag this issue came from: reporting an inference as
    // if it were a measurement.
    const wchar_t* ownCopy = (anyForeign && AnotherFullInstanceIsRunning())
        ? L" -- another copy of MDropDX12 is running and may be holding them"
        : L"";

    wchar_t msg[512];
    if (anyForeign && lost.size() == 1)
        FormatTo(msg, L"%s is in use by another application%s",
                 combos.c_str(), ownCopy);
    else if (anyForeign)
        FormatTo(msg, L"%d global hotkeys are in use by another application: %s%s",
                 (int)lost.size(), combos.c_str(), ownCopy);
    else
        FormatTo(msg, L"%s could not be registered: %s",
                 lost.size() == 1 ? L"A global hotkey" : L"Global hotkeys",
                 combos.c_str());
    AddError(msg, 8.0f, ERR_MISC, true);
}

std::vector<GlobalHotkeyState> Engine::GlobalHotkeyStates() const
{
    std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
    return m_globalHotkeyState;   // by value: a reference would outlive the lock
}

bool Engine::IsGlobalHotkeyHeld(int id) const
{
    std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
    for (const auto& st : m_globalHotkeyState)
        if (st.id == id) return st.held;
    return false;
}

bool Engine::GlobalHotkeyStatus(int id, bool* pHeld, DWORD* pErr) const
{
    std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
    for (const auto& st : m_globalHotkeyState) {
        if (st.id != id) continue;
        if (pHeld) *pHeld = st.held;
        if (pErr)  *pErr  = st.lastError;
        return true;
    }
    if (pHeld) *pHeld = false;
    if (pErr)  *pErr  = 0;
    return false;   // no entry: nothing global was ever asked for this action
}

bool Engine::HasGlobalHotkeyFailure(int id, DWORD* pErr) const
{
    std::lock_guard<std::mutex> lk(m_globalHotkeyStateMutex);
    for (const auto& st : m_globalHotkeyState) {
        if (st.id != id) continue;
        if (st.held) return false;
        if (pErr) *pErr = st.lastError;
        return true;
    }
    return false;   // no entry means no global binding was asked for
}

// The combinations worth offering. Modifier sets first, then keys.
//
// Deliberately NOT every possibility: a global hotkey needs at least two
// modifiers to be worth having, and probing all 256 virtual keys against all
// 15 modifier sets would be 3,840 registrations to produce a list nobody can
// read. These 7 x 60 = 420 are the combinations a person would actually reach
// for.
//
// Win-key sets are included but come with a caveat the UI has to state: the
// shell reserves many of them, and RegisterHotKey can succeed while Explorer
// still swallows the key. "Free" here means Windows let us register it, which
// is exactly what a real binding gets -- so the list does not overpromise, but
// a Win+key that reads free may still never arrive.
static const UINT kScanMods[] = {
    MOD_CONTROL | MOD_ALT,
    MOD_CONTROL | MOD_SHIFT,
    MOD_ALT     | MOD_SHIFT,
    MOD_CONTROL | MOD_ALT | MOD_SHIFT,
    MOD_WIN     | MOD_CONTROL,
    MOD_WIN     | MOD_ALT,
    MOD_WIN     | MOD_SHIFT,
};

std::vector<HotkeyScanRow> Engine::ScanGlobalHotkeyAvailability()
{
    // What we already hold, so ours are labelled rather than probed. Probing
    // one would answer "taken" -- true, and useless: a second registration of
    // a combination fails inside one process exactly as it does across two.
    struct Owned { UINT mod, vk; int id; };
    std::vector<Owned> mine;
    for (int i = 0; i < NUM_HOTKEYS; i++)
        if (m_hotkeys[i].globalVK != 0)
            mine.push_back({ m_hotkeys[i].globalMod, m_hotkeys[i].globalVK,
                             m_hotkeys[i].id });
    for (const auto& uh : m_userHotkeys)
        if (uh.vk != 0 && uh.scope == HKSCOPE_GLOBAL)
            mine.push_back({ uh.modifiers, uh.vk, uh.id });

    std::vector<UINT> keys;
    for (UINT k = 'A'; k <= 'Z'; k++) keys.push_back(k);
    for (UINT k = '0'; k <= '9'; k++) keys.push_back(k);
    for (UINT k = VK_F1; k <= VK_F24; k++) keys.push_back(k);

    // A probe id nothing else uses: built-in actions are below 1000 and user
    // hotkeys start there, so this range cannot collide with a live binding
    // even for the instant a probe is registered.
    const int kProbeIdBase = 0x4000;
    int probeId = kProbeIdBase;

    std::vector<HotkeyScanRow> out;
    out.reserve(_countof(kScanMods) * keys.size());

    for (UINT mod : kScanMods) {
        for (UINT vk : keys) {
            HotkeyScanRow row{ mod, vk, HKAVAIL_FREE, -1, 0 };

            bool isOurs = false;
            for (const auto& o : mine)
                if (o.mod == mod && o.vk == vk) {
                    row.status = HKAVAIL_OURS;
                    row.ownerId = o.id;
                    isOurs = true;
                    break;
                }
            if (isOurs) { out.push_back(row); continue; }

            // hWnd = NULL registers against the CALLING THREAD, which is what
            // makes this callable from the tool window's thread. Passing the
            // render window here would fail with ERROR_WINDOW_OF_OTHER_THREAD
            // and report every combination as taken.
            const int id = probeId++;
            SetLastError(0);
            if (RegisterHotKey(NULL, id, mod | MOD_NOREPEAT, vk)) {
                // Released IMMEDIATELY, not at the end of the scan. Holding
                // 434 combinations at once would take every free global key on
                // the machine away from whatever the user meant to use it for,
                // and an early return anywhere in this loop would leave them
                // that way for the rest of the session.
                UnregisterHotKey(NULL, id);
                row.status = HKAVAIL_FREE;
            } else {
                row.status = HKAVAIL_TAKEN;
                row.lastError = GetLastError();
            }
            out.push_back(row);
        }
    }
    return out;
}

std::wstring Engine::HotkeyOwnerName(int id) const
{
    for (int i = 0; i < NUM_HOTKEYS; i++)
        if (m_hotkeys[i].id == id) return m_hotkeys[i].szAction;
    for (const auto& uh : m_userHotkeys)
        if (uh.id == id) return uh.label.empty() ? L"(unnamed)" : uh.label;
    return L"";
}

std::wstring Engine::DescribeHotkeyConflicts(int excludeBuiltIn, int excludeUserId,
                                             UINT modifiers, UINT vk, HotkeyScope scope,
                                             int* pCount) const
{
    if (pCount) *pCount = 0;
    if (vk == 0) return L"";

    // Local and global are separate namespaces. One is resolved by Windows
    // and the other by the WM_KEYDOWN lookup, so the same keys can serve both
    // without either shadowing the other -- comparing across them would report
    // clashes that do not exist and unbind things nobody asked to lose.
    std::vector<std::wstring> names;
    for (int i = 0; i < NUM_HOTKEYS; i++) {
        if (m_hotkeys[i].id == excludeBuiltIn) continue;
        const bool hit = (scope == HKSCOPE_GLOBAL)
            ? (m_hotkeys[i].globalVK == vk && m_hotkeys[i].globalMod == modifiers)
            : (m_hotkeys[i].vk == vk && m_hotkeys[i].modifiers == modifiers);
        if (hit) names.push_back(m_hotkeys[i].szAction);
    }
    for (const auto& uh : m_userHotkeys) {
        if (uh.id == excludeUserId) continue;
        if (uh.vk == vk && uh.modifiers == modifiers && uh.scope == scope)
            names.push_back(uh.label.empty() ? L"(unnamed)" : uh.label);
    }

    if (pCount) *pCount = (int)names.size();
    if (names.empty()) return L"";

    std::wstring list;
    for (size_t i = 0; i < names.size(); i++) {
        if (i) list += (i + 1 == names.size()) ? L" and " : L", ";
        list += L"\"" + names[i] + L"\"";
    }
    // A STATEMENT, with no question attached. The same sentence has to serve
    // an inline label in the edit dialog, a confirm box, and an IPC record;
    // only the confirm wants "...and unbind it?" on the end, so it appends
    // that itself rather than the other two having to strip it off.
    return FormatHotkeyDisplay(modifiers, vk) +
           (scope == HKSCOPE_GLOBAL ? L" is already the global hotkey for "
                                    : L" is already the hotkey for ") + list + L".";
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
        if (!m_bPresetLockedByCode) {
            RenderCommand cmd;
            cmd.cmd = RenderCmd::PrevPreset;
            cmd.fParam = 0.0f; // hard cut
            EnqueueRenderCmd(std::move(cmd));
        }
        return true;
    case HK_HARD_CUT:
        if (!m_bPresetLockedByCode) {
            RenderCommand cmd;
            cmd.cmd = RenderCmd::NextPreset;
            cmd.fParam = 0.0f; // hard cut
            EnqueueRenderCmd(std::move(cmd));
            m_fHardCutThresh *= 2.0f;
        }
        return true;
    case HK_RANDOM_MASHUP: {
        bool bCompLock = m_bCompShaderLock;
        bool bWarpLock = m_bWarpShaderLock;
        m_bCompShaderLock = false; m_bWarpShaderLock = false;
        m_pszNextLoadReason = "hotkey"; LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bCompShaderLock = true; m_bWarpShaderLock = false;
            m_pszNextLoadReason = "hotkey"; LoadRandomPreset(0.0f);
            if (WaitForPendingLoad(3000)) {
                m_bCompShaderLock = false; m_bWarpShaderLock = true;
                m_pszNextLoadReason = "hotkey"; LoadRandomPreset(0.0f);
                WaitForPendingLoad(3000);
            }
        }
        m_bCompShaderLock = bCompLock;
        m_bWarpShaderLock = bWarpLock;
        return true;
    }
    case HK_LOCK_PRESET:
        SetUserPresetLock(!m_bPresetLockedByUser);
        AddNotification(m_bPresetLockedByUser ? L"Preset locked" : L"Preset unlocked");
        SendSettingsInfoToMDropDX12Remote();
        return true;
    case HK_TOGGLE_RANDOM:
        // Through the one writer, so the change takes the value back off a
        // display profile's session-only loan (#356). A bare flip left the
        // loan standing, and both the profile save and the shutdown save then
        // wrote the profile's value rather than the one just chosen.
        SetMainRenderOrder(!m_bSequentialPresetOrder);
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
                FormatTo(m_szPresetDir, L"%spresets\\", m_szMilkdrop2Path);
                TryDescendIntoPresetSubdirHelper(m_szPresetDir);
                Config().SetString(L"Settings", L"szPresetDir", m_szPresetDir);
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
            CopyTo(m_waitstring.szText, m_pState->m_szDesc);
            wasabiApiLangString(IDS_SAVE_AS, m_waitstring.szPrompt);
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
    // The way IN to the settings overlay, which had none: the screen was drawn
    // in full and nothing anywhere assigned UI_SETTINGS, so it could not be
    // opened and its own header was the only clue it existed (#163).
    //
    // Shaped exactly like HK_OPEN_MENU above, including which modes it will
    // leave. Entering only from a mode that is showing the frame or another
    // full-screen list is what keeps it from stealing a half-typed filename:
    // UI_SAVEAS and the rest run a waitstring, and dropping that on the floor
    // would lose what the user was in the middle of writing. Escape returns to
    // UI_REGULAR from here -- engine_input.cpp already lists UI_SETTINGS among
    // the modes it closes.
    case HK_SETTINGS_OVERLAY:
        m_show_help = 0;
        if (m_UI_mode == UI_SETTINGS)
            m_UI_mode = UI_REGULAR;
        else if (m_UI_mode == UI_REGULAR || m_UI_mode == UI_MENU || m_UI_mode == UI_LOAD)
            m_UI_mode = UI_SETTINGS;
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
    // The three HUD toggles relay their RESOLVED state to the children, so
    // one keypress moves every screen rather than only the primary's. Sent
    // after the flip, not as a toggle: a child that missed a message would
    // otherwise end up showing the inverse of what the parent shows.
    case HK_SHOW_PRESET_INFO:
        m_bShowPresetInfo = !m_bShowPresetInfo;
        RelayHudStateToChildren();
        return true;
    case HK_SHOW_FPS:
        m_bShowFPS = !m_bShowFPS;
        RelayHudStateToChildren();
        return true;
    case HK_SHOW_BATTERY:
        SetShowBattery(!m_bShowBattery);
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
    case HK_OPEN_CUSTOM_SHADERS:
        OpenCustomShadersWindow();
        return true;
    case HK_TOGGLE_SHADER_OVERRIDES: {
        // Master enable for tag-selected shader overrides. Re-resolves for the
        // running preset so turning it off restores that preset's own shaders
        // straight away rather than at the next preset change.
        ShaderOverrideStore& store = ShaderOverrides();
        const bool on = !store.IsEnabled();
        store.SetEnabled(on);
        store.Save();
        ResolveShaderOverrideForPreset(m_pState);
        RequestShaderRecompile();
        AddNotification(on ? L"Shader overrides ON" : L"Shader overrides OFF");
        if (m_pCustomShadersWindow) m_pCustomShadersWindow->RefreshAll();
        return true;
    }
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
    case HK_MIXER_GROUP1_UP:     MixerGroupHotkey(0,  MixerVolumeStep(), false); return true;
    case HK_MIXER_GROUP1_DOWN:   MixerGroupHotkey(0, -MixerVolumeStep(), false); return true;
    case HK_MIXER_GROUP1_MUTE:   MixerGroupHotkey(0, 0.0f, true); return true;

    case HK_MIXER_SONAR_RESTART: {
      // Fires immediately unless the user switched the guard on. A second
      // press inside the window confirms; anything else lets it lapse, and an
      // expired window drops the action rather than performing it late.
      if (MixerConfirmPending()) {
        if (MixerConfirmAnswer(true)) {
          std::wstring detail;
          const bool ok = mdrop::RestartSonar(detail);
          AddNotification((wchar_t*)(ok ? L"Restarting SteelSeries"
                                        : L"SteelSeries restart failed"));
        }
        return true;
      }
      if (!MixerConfirmBegin(L"restart SteelSeries")) {
        std::wstring detail;
        const bool ok = mdrop::RestartSonar(detail);
        AddNotification((wchar_t*)(ok ? L"Restarting SteelSeries"
                                      : L"SteelSeries restart failed"));
      } else {
        AddNotification((wchar_t*)L"Press again to restart SteelSeries");
      }
      return true;
    }

    case HK_OPEN_AUDIO_MIXER: OpenAudioMixerWindow(); return true;
    case HK_SAVE_DISPLAY_SNAPSHOT: {
        const std::wstring path = SaveDisplayProfileSnapshot();
        if (path.empty()) {
            AddNotification((wchar_t*)L"Could not save display snapshot");
        } else {
            // The file name, not the whole path: it is a timestamp, which is
            // the part worth reading, and the path is long enough to run off
            // the screen.
            const size_t slash = path.find_last_of(L"\\/");
            std::wstring leaf = (slash == std::wstring::npos)
                                    ? path : path.substr(slash + 1);
            AddNotification((wchar_t*)(L"Saved " + leaf).c_str());
        }
        return true;
    }
    case HK_LOAD_DEFAULT_PROFILE: LoadDefaultDisplayProfile(); return true;
    case HK_NEXT_DISPLAY_PROFILE: StepDisplayProfile(1);  return true;
    case HK_PREV_DISPLAY_PROFILE: StepDisplayProfile(-1); return true;

    case HK_OPEN_PRESET_EDITOR:
        // Opens on whatever preset is running -- no selection needed.
        OpenPresetEditorWindow();
        return true;
    case HK_POLL_TRACK_INFO:
        if (hRender) PostMessage(hRender, WM_MW_HOTKEY_ACTION, (WPARAM)actionId, 0);
        return true;
    case HK_MIRROR_WATERMARK:
        if (hRender) PostMessage(hRender, WM_MW_MIRROR_WM, 0, 0);
        return true;
    case HK_MIRROR_INDEPENDENT:
        ToggleMirrorIndependentRender();
        return true;
    case HK_WATERMARK:
        if (hRender) PostMessage(hRender, WM_MW_WATERMARK, 0, 0);
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
        CopyTo(szOldPreset, m_szCurrentPresetFile);
        m_bWarpShaderLock = false;
        m_pszNextLoadReason = "hotkey"; LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bWarpShaderLock = true;
            m_pszNextLoadReason = "hotkey"; LoadPreset(szOldPreset, 0.0f);
            WaitForPendingLoad(3000);
        }
        m_bWarpShaderLock = bWarpLock;
        return true;
    }
    case HK_SCRAMBLE_COMP: {
        bool bCompLock = m_bCompShaderLock;
        wchar_t szOldPreset[MAX_PATH];
        CopyTo(szOldPreset, m_szCurrentPresetFile);
        m_bCompShaderLock = false;
        m_pszNextLoadReason = "hotkey"; LoadRandomPreset(0.0f);
        if (WaitForPendingLoad(3000)) {
            m_bCompShaderLock = true;
            m_pszNextLoadReason = "hotkey"; LoadPreset(szOldPreset, 0.0f);
            WaitForPendingLoad(3000);
        }
        m_bCompShaderLock = bCompLock;
        return true;
    }
    case HK_QUICKSAVE:
        SaveCurrentPresetToQuicksave(false);
        return true;
    case HK_SCROLL_LOCK:
        SetUserPresetLock((GetKeyState(VK_SCROLL) & 1) != 0);
        TogglePlaylist();
        return true;
    case HK_RELOAD_MESSAGES:
        ReadCustomMessages();
        AddNotification(L"Messages reloaded");
        return true;

    // ── Misc ──
    case HK_DEBUG_INFO:
        m_bShowDebugInfo = !m_bShowDebugInfo;
        // Each process reports its OWN gpu3d/gpu share in this overlay, so
        // relaying it is what makes per-process GPU cost readable at a glance:
        // one figure per screen, from the process actually driving it.
        RelayHudStateToChildren();
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

    case HK_TOGGLE_MESSAGES:
        SetSpriteMessagesMode(m_nSpriteMessagesMode ^ 1); // toggle messages bit
        AddNotification(MessagesEnabled() ? L"Messages ON" : L"Messages OFF");
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

void Engine::LaunchOrFocusApp(const std::wstring& path, const std::wstring& args)
{
    if (path.empty()) {
        AddNotification(L"No app configured");
        return;
    }

    // With ARGUMENTS this always starts a new process, and never focuses one
    // that is already up.
    //
    // Focusing is the right answer for "open my music player" -- the app is a
    // place, and a second copy is not wanted. It is the wrong answer for
    // "run this command", which is what arguments turn the verb into: the
    // arguments ARE the request, and handing focus to a process started earlier
    // with different ones silently does nothing the caller asked for. The
    // comparison script relies on this -- it invokes a Python helper, and a
    // previous run's window must not stand in for the run being asked for.
    if (!args.empty()) {
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"open";
        sei.lpFile = path.c_str();
        sei.lpParameters = args.c_str();
        // SW_SHOWNORMAL, not hidden: a console helper that asks a question or
        // fails should be visible rather than leaving the script to look hung.
        sei.nShow  = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) {
            if (sei.hProcess) CloseHandle(sei.hProcess);
            DLOG_INFO("LAUNCH: started '%ls' with arguments '%ls'",
                      path.c_str(), args.c_str());
        } else {
            wchar_t msg[MAX_PATH + 64];
            swprintf(msg, MAX_PATH + 64, L"Could not launch %s (%lu)",
                     path.c_str(), GetLastError());
            AddNotification(msg);
            DLOG_ERROR("LAUNCH failed: '%ls' args '%ls' err=%lu",
                       path.c_str(), args.c_str(), GetLastError());
        }
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
    m_bIdleTimerEnabled = Config().GetInt(L"IdleTimer", L"Enabled", 0) != 0;
    m_nIdleTimeoutMinutes = Config().GetInt(L"IdleTimer", L"TimeoutMinutes", 5);
    if (m_nIdleTimeoutMinutes < 1) m_nIdleTimeoutMinutes = 1;
    if (m_nIdleTimeoutMinutes > 60) m_nIdleTimeoutMinutes = 60;
    m_nIdleAction = Config().GetInt(L"IdleTimer", L"Action", 0);
    if (m_nIdleAction < 0 || m_nIdleAction > 3) m_nIdleAction = 0;
    m_bIdleAutoRestore = Config().GetInt(L"IdleTimer", L"AutoRestore", 1) != 0;
    Config().GetStringTo(L"IdleTimer", L"Profile", L"", m_szIdleProfile);
    m_bIdleFullOpacity = Config().GetInt(L"IdleTimer", L"FullOpacity", 0) != 0;
    // The countdown is "time since the last input OR the last reset, whichever is
    // shorter", and with no reset yet issued the second term is the app's uptime.
    // Starting the baseline here means a freshly started app reads zero idle,
    // which is what just having been launched means.
    m_nIdleResetTick = GetTickCount();
}

void Engine::SaveIdleTimerSettings()
{
    wchar_t buf[64];

    Config().SetString(L"IdleTimer", L"Enabled", m_bIdleTimerEnabled ? L"1" : L"0");
    swprintf(buf, 64, L"%d", m_nIdleTimeoutMinutes);
    Config().SetString(L"IdleTimer", L"TimeoutMinutes", buf);
    swprintf(buf, 64, L"%d", m_nIdleAction);
    Config().SetString(L"IdleTimer", L"Action", buf);
    Config().SetString(L"IdleTimer", L"AutoRestore", m_bIdleAutoRestore ? L"1" : L"0");
    Config().SetString(L"IdleTimer", L"Profile", m_szIdleProfile);
    Config().SetString(L"IdleTimer", L"FullOpacity", m_bIdleFullOpacity ? L"1" : L"0");
}

DWORD Engine::IdleElapsedMs() const
{
    // Both terms are unsigned DELTAS, never raw ticks, so the 49.7-day
    // GetTickCount wrap cannot turn a short idle into a 49-day one. Comparing
    // lii.dwTime against m_nIdleResetTick directly would do exactly that.
    const DWORD now = GetTickCount();
    const DWORD sinceReset = now - m_nIdleResetTick;
    LASTINPUTINFO lii = { sizeof(lii) };
    if (!GetLastInputInfo(&lii)) return sinceReset;
    const DWORD sysIdle = now - lii.dwTime;
    return (sysIdle < sinceReset) ? sysIdle : sinceReset;
}

std::wstring Engine::FormatHotkeyDisplay(UINT modifiers, UINT vk) const
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
            wcsncpy_s(p, (size_t)rem, line, _TRUNCATE);
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

// ── Hotkeys over IPC: what is asked for, and what Windows actually granted ──
//
// A global binding is a REQUEST. RegisterHotKey is first-come-first-served
// across the whole machine, so these answer the question the Hotkeys window
// could not: is this combination actually ours (forgejo#72)? They also make
// the feature testable with no window open and no relaunch, which is the only
// way to drive it that does not interrupt whoever is at the desk.
//
// An early-return helper rather than three more links in LaunchMessage's
// else-if chain, which is at MSVC's block-nesting limit -- adding them there
// is error C1061, not a style question. Same reason HandlePresetCycleIPC and
// HandleCanvasIPC exist. Returns false for anything it does not recognise, so
// the fallback that hands unmatched messages to the script engine is
// untouched.
//
// Runs on the WINDOW's thread: PipeServer::Start posts WM_MW_IPC_MESSAGE to
// hwnd, which is the same thread that owns the hotkey registrations, so
// re-registering inline here is correct.
bool Engine::HandleHotkeyDiagIPC(const wchar_t* sMessage)
{
  extern PipeServer g_pipeServer;

  if (MSG_IS(sMessage, L"DIAG_HOTKEY_BIND=")) {
    // DIAG_HOTKEY_BIND=<iniKey>,<mod>,<vk>[,<scope>] -- sets a binding.
    //
    // Scope 1 (global) is the default and was for a long time the only thing
    // this verb could reach, which left the LOCAL half of every action with
    // no IPC path at all -- so a test that meant to bind Alt+B locally set
    // the global binding instead and then failed for the wrong reason.
    // Scope 0 writes modifiers/vk, the pair the window-message lookup reads.
    //
    // In memory ONLY: deliberately no SaveHotkeysJson. A probe that drives
    // bindings must not be able to leave one behind in the user's file, and
    // testing mode does not cover hotkeys.json -- the write shield is for the
    // ini. vk=0 unbinds, which is how a test puts things back.
    const wchar_t* args = sMessage + (_countof(L"DIAG_HOTKEY_BIND=") - 1);
    std::wstring key, rest;
    if (const wchar_t* comma = wcschr(args, L',')) {
      key.assign(args, comma);
      rest = comma + 1;
    } else {
      key = args;
    }
    UINT mod = 0, vk = 0;
    int scope = HKSCOPE_GLOBAL;
    if (!rest.empty()) {
      mod = (UINT)_wtoi(rest.c_str());
      if (const wchar_t* c2 = wcschr(rest.c_str(), L',')) {
        vk = (UINT)_wtoi(c2 + 1);
        if (const wchar_t* c3 = wcschr(c2 + 1, L','))
          scope = _wtoi(c3 + 1) ? HKSCOPE_GLOBAL : HKSCOPE_LOCAL;
      }
    }

    int found = -1;
    for (int i = 0; i < NUM_HOTKEYS; i++)
      if (_wcsicmp(m_hotkeys[i].szIniKey, key.c_str()) == 0) { found = i; break; }

    wchar_t buf[512];
    if (found < 0) {
      FormatTo(buf, L"HOTKEY_BIND|key=%s|ERROR=no such action", key.c_str());
      g_pipeServer.Send(buf);
      return true;
    }

    if (scope == HKSCOPE_LOCAL) {
      // No re-registration: a local binding is answered by the window
      // procedure's own lookup and never goes near RegisterHotKey.
      m_hotkeys[found].modifiers = mod;
      m_hotkeys[found].vk        = vk;
      FormatTo(buf, L"HOTKEY_BIND|key=%s|mod=%u|vk=%u|combo=%s|scope=local"
                    L"|held=%d|err=0",
               m_hotkeys[found].szIniKey, mod, vk,
               vk ? FormatHotkeyDisplay(mod, vk).c_str() : L"(none)",
               vk ? 1 : 0);
      g_pipeServer.Send(buf);
      return true;
    }

    m_hotkeys[found].globalMod = mod;
    m_hotkeys[found].globalVK  = vk;

    // Marshalled, not called. This runs on the render thread, and
    // RegisterHotKey demands the thread that owns the window -- calling it
    // from here failed every binding with ERROR_WINDOW_OF_OTHER_THREAD and
    // recorded them all as stolen.
    //
    // SEND rather than post, because the reply has to carry the outcome, and
    // with a TIMEOUT rather than a bare SendMessage: this is the render thread
    // blocking on the window thread, and a diagnostic must not be able to
    // wedge the frame loop if that thread is busy. SMTO_ABORTIFHUNG gives up
    // on a hung window rather than waiting out the full timeout.
    HWND hw = GetPluginWindow();
    DWORD_PTR unused = 0;
    if (!SendMessageTimeoutW(hw, WM_MW_REGISTER_HOTKEYS, 0, 0,
                             SMTO_ABORTIFHUNG, 2000, &unused)) {
      FormatTo(buf, L"HOTKEY_BIND|key=%s|ERROR=re-register timed out",
               m_hotkeys[found].szIniKey);
      g_pipeServer.Send(buf);
      return true;
    }

    // READ from the registration state, never inferred from the absence of a
    // recorded failure. This used to be
    //
    //     const bool failed = HasGlobalHotkeyFailure(id, &err);
    //     const bool held   = (vk != 0) && !failed;
    //
    // which answers "held" whenever nothing went wrong -- and a pass that
    // never ran cannot have gone wrong. On a test instance, where registration
    // was skipped outright, that reported holding every combination the app
    // had never once asked Windows for (#173). Absence of evidence, read as
    // evidence: the same "table of lies" the wrong-thread guard above refuses
    // to write.
    bool held = false;
    DWORD err = 0;
    const bool known = GlobalHotkeyStatus(m_hotkeys[found].id, &held, &err);
    FormatTo(buf, L"HOTKEY_BIND|key=%s|mod=%u|vk=%u|combo=%s|scope=global"
                  L"|held=%d|err=%lu|known=%d|mode=%s",
             m_hotkeys[found].szIniKey, mod, vk,
             vk ? FormatHotkeyDisplay(mod, vk).c_str() : L"(none)",
             held ? 1 : 0, (unsigned long)err, known ? 1 : 0,
             GlobalHotkeyModeName(GlobalHotkeyRegistrationMode()));
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_HOTKEY_CONFLICTS=")) {
    // DIAG_HOTKEY_CONFLICTS=<iniKey>,<mod>,<vk>,<scope> -- what assigning this
    // combination would UNBIND. The engine builds the sentence and the confirm
    // dialog shows that same string, so the two cannot disagree about what is
    // at stake.
    const wchar_t* args = sMessage + (_countof(L"DIAG_HOTKEY_CONFLICTS=") - 1);
    std::wstring parts[4];
    int n = 0;
    for (const wchar_t* p = args; n < 4; ) {
      const wchar_t* comma = wcschr(p, L',');
      parts[n++].assign(p, comma ? comma : p + wcslen(p));
      if (!comma) break;
      p = comma + 1;
    }
    const UINT mod = (UINT)_wtoi(parts[1].c_str());
    const UINT vk  = (UINT)_wtoi(parts[2].c_str());
    const HotkeyScope scope = (_wtoi(parts[3].c_str()) != 0) ? HKSCOPE_GLOBAL
                                                             : HKSCOPE_LOCAL;
    int self = -1;
    for (int i = 0; i < NUM_HOTKEYS; i++)
      if (_wcsicmp(m_hotkeys[i].szIniKey, parts[0].c_str()) == 0) {
        self = m_hotkeys[i].id;
        break;
      }

    int count = 0;
    const std::wstring text = DescribeHotkeyConflicts(self, -1, mod, vk, scope, &count);
    // Newlines belong to the dialog, not to a one-line IPC record.
    std::wstring flat = text;
    for (auto& c : flat) if (c == L'\n' || c == L'\r') c = L' ';
    wchar_t buf[1024];
    FormatTo(buf, L"HOTKEY_CONFLICTS|n=%d|text=%s", count, flat.c_str());
    g_pipeServer.Send(buf);
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_HOTKEY_SCAN")) {
    // Runs here on the render thread, which is fine: the probes pass
    // hWnd = NULL and so belong to whichever thread asks.
    const std::vector<HotkeyScanRow> rows = ScanGlobalHotkeyAvailability();
    int free = 0, taken = 0, ours = 0;
    for (const auto& r : rows)
      (r.status == HKAVAIL_FREE ? free : r.status == HKAVAIL_OURS ? ours : taken)++;

    wchar_t buf[512];
    FormatTo(buf, L"HOTKEY_SCAN_BEGIN|total=%d,free=%d,taken=%d,ours=%d",
             (int)rows.size(), free, taken, ours);
    g_pipeServer.Send(buf);

    for (const auto& r : rows) {
      const wchar_t* status = r.status == HKAVAIL_FREE  ? L"free"
                            : r.status == HKAVAIL_OURS  ? L"ours"
                                                        : L"taken";
      const std::wstring owner = r.ownerId >= 0 ? HotkeyOwnerName(r.ownerId)
                                                : std::wstring();
      FormatTo(buf, L"HOTKEY_SCAN|mod=%u|vk=%u|combo=%s|status=%s|action=%s|err=%lu",
               r.modifiers, r.vk,
               FormatHotkeyDisplay(r.modifiers, r.vk).c_str(),
               status, owner.c_str(), (unsigned long)r.lastError);
      g_pipeServer.Send(buf);
    }
    g_pipeServer.Send(L"HOTKEY_SCAN_END");
    return true;
  }

  if (MSG_IS(sMessage, L"DIAG_HOTKEYS")) {
    // Only GLOBAL bindings are listed. A local key never reaches
    // RegisterHotKey and so cannot collide with another process -- listing it
    // with held=0 would read as a failure when nothing was ever asked of
    // Windows.
    const std::vector<GlobalHotkeyState> states = GlobalHotkeyStates();
    int held = 0, failed = 0;
    for (const auto& st : states) (st.held ? held : failed)++;

    // mode= says which of the three things the last pass did, so a reader can
    // tell "we hold this" from "this was available when we asked". Without it
    // held=1 means two different things on two different instances and the
    // reply cannot be interpreted without knowing which one sent it.
    wchar_t buf[1024];
    FormatTo(buf, L"HOTKEYS|global=%d,held=%d,failed=%d,child=%d,mode=%s",
             (int)states.size(), held, failed, m_bChildMode ? 1 : 0,
             GlobalHotkeyModeName(GlobalHotkeyRegistrationMode()));
    g_pipeServer.Send(buf);

    for (const auto& st : states) {
      const wchar_t* iniKey = nullptr;
      const wchar_t* label  = nullptr;
      for (int i = 0; i < NUM_HOTKEYS; i++)
        if (m_hotkeys[i].id == st.id) {
          iniKey = m_hotkeys[i].szIniKey;
          label  = m_hotkeys[i].szAction;
          break;
        }
      if (iniKey) {
        FormatTo(buf, L"HOTKEY|key=%s|label=%s|mod=%u|vk=%u|combo=%s|held=%d|err=%lu"
                      L"|probed=%d",
                 iniKey, label, st.modifiers, st.vk,
                 FormatHotkeyDisplay(st.modifiers, st.vk).c_str(),
                 st.held ? 1 : 0, (unsigned long)st.lastError, st.probed ? 1 : 0);
      } else {
        const wchar_t* uLabel = L"(unnamed)";
        for (const auto& uh : m_userHotkeys)
          if (uh.id == st.id && !uh.label.empty()) { uLabel = uh.label.c_str(); break; }
        FormatTo(buf, L"HOTKEY|user=%d|label=%s|mod=%u|vk=%u|combo=%s|held=%d|err=%lu"
                      L"|probed=%d",
                 st.id, uLabel, st.modifiers, st.vk,
                 FormatHotkeyDisplay(st.modifiers, st.vk).c_str(),
                 st.held ? 1 : 0, (unsigned long)st.lastError, st.probed ? 1 : 0);
      }
      g_pipeServer.Send(buf);
    }
    g_pipeServer.Send(L"HOTKEYS_END");
    return true;
  }

  return false;
}

} // namespace mdrop
