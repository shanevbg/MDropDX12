/*
  LICENSE
  -------
Copyright 2005-2013 Nullsoft, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice,
    this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above copyright notice,
    this list of conditions and the following disclaimer in the documentation
    and/or other materials provided with the distribution.

  * Neither the name of Nullsoft nor the names of its contributors may be used to
    endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef MDROP_ENGINE_H
#define MDROP_ENGINE_H 1

// =========================================================
// SPOUT & DISPLAY OUTPUTS
#include "SpoutDX12.h" // Spout2 DX12 support class (D3D11On12 interop)
#include "display_output.h"
#include "hotkeys.h"
#include <io.h> // for file existence check
// =========================================================

#include "engineshell.h"
#include "engine_helpers.h"  // SETTINGS_NUM_PAGES, control IDs
#include "md_defines.h"
#include "menu.h"
#include "support.h"
#include "texmgr.h"
#include "state.h"
#include "dx12helpers.h"  // DX12Texture
#include "video_capture.h" // VideoCaptureSource (needed for unique_ptr complete type)
#include "midi_input.h"   // MidiInput, MidiRow (needed for MIDI members)
#include "tool_window.h"  // DisplaysWindow (needed for unique_ptr complete type)
#include <vector>
#include <array>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <string>
#include <regex>
#include "../ns-eel2/ns-eel.h"
#include "mdropdx12.h"

//#include <core/sdk/IPlaybackService.h>

extern "C" int (*warand)(void);

namespace mdrop {

struct ScriptState {
  std::vector<std::wstring> lines;  // parsed non-comment lines
  int currentLine = -1;             // -1 = not playing
  bool playing = false;
  bool loop = false;
  double bpm = 120.0;
  int beats = 4;                    // beats before next line
  double lastLineTime = 0.0;       // GetTime() when last line executed
  // Default message style
  std::wstring defaultFont = L"Arial";
  int defaultSize = 20;
  int defaultR = 255, defaultG = 255, defaultB = 255;
  std::wstring filePath;            // current script file path
};

struct WindowTitleProfile {
    wchar_t szName[64] = {};           // Profile name (e.g., "Spotify")
    wchar_t szWindowRegex[256] = {};   // Regex to match window title
    wchar_t szParseRegex[512] = {};    // Regex with named groups: (?<artist>...) (?<title>...) (?<album>...)
    int nPollIntervalSec = 2;          // Poll interval in seconds (1-10)
};

typedef enum { TEX_DISK, TEX_VS, TEX_FEEDBACK, TEX_IMAGE_FEEDBACK, TEX_AUDIO, TEX_BUFFER_B, TEX_BLUR0, TEX_BLUR1, TEX_BLUR2, TEX_BLUR3, TEX_BLUR4, TEX_BLUR5, TEX_BLUR6, TEX_BLUR_LAST } tex_code;
typedef enum { UI_REGULAR, UI_MENU, UI_LOAD, UI_LOAD_DEL, UI_LOAD_RENAME, UI_SAVEAS, UI_SAVE_OVERWRITE, UI_EDIT_MENU_STRING, UI_CHANGEDIR, UI_IMPORT_WAVE, UI_EXPORT_WAVE, UI_IMPORT_SHAPE, UI_EXPORT_SHAPE, UI_UPGRADE_PIXEL_SHADER, UI_MASHUP, UI_SETTINGS } ui_mode;
typedef struct { float rad; float ang; float a; float c; } td_vertinfo; // blending: mix = max(0,min(1,a*t + c));
typedef char* CHARPTR;
LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

#define MY_FFT_SAMPLES 512     // for old [pre-vms] milkdrop sound analysis
struct td_mysounddata {
  float   imm[3];			// bass, mids, treble (absolute)
  float	  imm_rel[3];		// bass, mids, treble (relative to song; 1=avg, 0.9~below, 1.1~above)
  float	  avg[3];			// bass, mids, treble (absolute)
  float	  avg_rel[3];		// bass, mids, treble (relative to song; 1=avg, 0.9~below, 1.1~above)
  float	  long_avg[3];	// bass, mids, treble (absolute)
  float   fWave[2][576];
  float   fSpecLeft[MY_FFT_SAMPLES];
  float   fSpecRight[MY_FFT_SAMPLES];
  static const int RECENT_BUF_MAX = 4096;
  float   recent_buf[3][RECENT_BUF_MAX];
  int     recent_pos[3];
  int     recent_len[3];
  float	  smooth[3];
  float	  smooth_rel[3];
};

typedef struct {
  int 	bActive;
  int 	bFilterBadChars;	// if true, it will filter out any characters that don't belong in a filename, plus the & symbol (because it doesn't display properly with DrawText)
  int 	bDisplayAsCode;		// if true, semicolons will be followed by a newline, for display
  int		nMaxLen;			// can't be more than 511
  int		nCursorPos;
  int		nSelAnchorPos;		// -1 if no selection made
  int 	bOvertypeMode;
  wchar_t	szText[48000];
  wchar_t	szPrompt[512];
  wchar_t	szToolTip[512];
  char	szClipboard[48000];
  wchar_t	szClipboardW[48000];
} td_waitstr;

typedef struct {
  int 	bBold;
  int 	bItal;
  wchar_t	szFace[128];
  int		nColorR;    // 0..255
  int		nColorG;    // 0..255
  int		nColorB;    // 0..255
}
td_custom_msg_font;

enum {
  MD2_PS_NONE = 0,
  MD2_PS_2_0 = 2,
  MD2_PS_2_X = 3,
  MD2_PS_3_0 = 4,
  MD2_PS_4_0 = 5, // not supported by milkdrop
  MD2_PS_5_0 = 6, // SM5.0 for Shadertoy (.milk3) presets
};

typedef struct {
  int		nFont;
  float	fSize;	// 0..100
  float	x;
  float	y;
  float	randx;
  float randy;
  float	growth;
  float	fTime;	// total time to display the message, in seconds
  float	fFade;	// % (0..1) of the time that is spent fading in
  float	fFadeOut;
  float	fBurnTime;

  // overrides
  int     bOverrideBold;
  int     bOverrideItal;
  int     bOverrideFace;
  int     bOverrideColorR;
  int     bOverrideColorG;
  int     bOverrideColorB;
  int	    nColorR;    // 0..255
  int	    nColorG;    // 0..255
  int	    nColorB;    // 0..255
  int     nRandR;
  int     nRandG;
  int  	  nRandB;
  int     bBold;
  int     bItal;
  wchar_t szFace[128];

  wchar_t	szText[256];

  // Per-message randomization flags (0=off, 1=on)
  int bRandPos;
  int bRandSize;
  int bRandFont;
  int bRandColor;
  int bRandEffects;
  int bRandGrowth;
  int bRandDuration;

  int nAnimProfile;  // -1 = use message's own settings, -2 = random profile, 0+ = named profile index
}
td_custom_msg;

#define MAX_ANIM_PROFILES 32

struct td_anim_profile {
  wchar_t szName[64] = {};        // profile name for UI (e.g. "Slide from Left")
  bool    bEnabled = true;        // included in randomization pool

  // Position
  float   fX = 0.5f, fY = 0.5f;  // target position (0..1)
  float   fRandX = 0.0f, fRandY = 0.0f;  // random offset ranges

  // Entry animation
  float   fStartX = -100.0f;     // start X (-100 = no slide)
  float   fStartY = -100.0f;     // start Y (-100 = no slide)
  float   fMoveTime = 0.0f;      // slide-in duration (seconds)
  int     nEaseMode = 2;         // 0=linear, 1=ease-in, 2=ease-out
  float   fEaseFactor = 2.0f;    // easing intensity

  // Appearance
  wchar_t szFontFace[128] = {};  // empty = use default
  float   fFontSize = 50.0f;     // 0..100
  int     bBold = 0, bItal = 0;
  int     nColorR = 255, nColorG = 255, nColorB = 255;
  int     nRandR = 0, nRandG = 0, nRandB = 0;

  // Timing
  float   fDuration = 5.0f;      // total display time
  float   fFadeIn = 0.2f;        // fade-in fraction (0..1)
  float   fFadeOut = 0.5f;       // fade-out time (seconds)
  float   fBurnTime = 0.0f;      // burn/flare effect

  // Effects
  float   fGrowth = 1.0f;        // text scale-over-time
  float   fShadowOffset = 0.0f;  // shadow distance
  float   fBoxAlpha = 0.0f;      // background box opacity (0=none)
  int     nBoxColR = 0, nBoxColG = 0, nBoxColB = 0;

  // Per-trigger randomization flags
  int     bRandPos = 0, bRandSize = 0, bRandColor = 0;
  int     bRandGrowth = 0, bRandDuration = 0;
};

typedef struct td_supertext {
  float	fStartTime = -1.0f; // off state
  int 	bRedrawSuperText;	// true if it needs redraw
  int 	bIsSongTitle;		// false for custom message, true for song title
  //char	szText[256];
  wchar_t	szTextW[512];
  wchar_t	nFontFace[128];
  int 	bBold;
  int 	bItal;
  float fMoveTime = -1;
  float	fStartX = - 100;
  float fStartY = - 100;
  float	fX;
  float fY;
  float	fFontSize;			// [0..100] for custom messages, [0..4] for song titles
  float fGrowth;			// applies to custom messages only
  int		nFontSizeUsed;		// height IN PIXELS
  float	fDuration;
  float	fFadeInTime; // applies to custom messages only; song title fade times are handled specially
  float	fFadeOutTime; // applies to custom messages only; song title fade times are handled specially
  int  	nColorR;
  int   nColorG;
  int  	nColorB;
  int   nEaseMode = 2;	// 0 = linear, 1 = ease-in, 2 = ease-out (default)
  float fEaseFactor = 2.0f; // 1.0f = linear, 2.0f = ease-in/out, 3.0f = more pronounced ease-in/out
  float fShadowOffset = 2.0f;
  float fBurnTime; // seconds
  float fBoxAlpha = 0.0f; // 0 = transparent, 255 = opaque
  int fBoxColR = 0;
  int fBoxColG = 0;
  int fBoxColB = 0;
  float fBoxLeft = 1.0f;
  float fBoxRight = 1.0f;
  float fBoxTop = 1.0f;
  float fBoxBottom = 1.0f;
}
td_supertext;

typedef struct {
  wchar_t        texname[256];   // ~filename, but without path or extension!
  LPDIRECT3DBASETEXTURE9 texptr;
  int                w, h, d;
  //D3DXHANDLE         texsize_param;
  bool               bEvictable;
  int                 nAge;   // only valid if bEvictable is true
  int                 nSizeInBytes;    // only valid if bEvictable is true
  DX12Texture        dx12Tex;         // DX12 GPU resource + SRV
} TexInfo;

typedef struct {
  std::wstring    texname;  // just for ref
  D3DXHANDLE texsize_param;
  int        w, h;
} TexSizeParamInfo;

typedef struct SamplerInfo {
  LPDIRECT3DBASETEXTURE9 texptr;
  bool               bBilinear;
  bool               bWrap;
  UINT               dx12SrvIndex = UINT_MAX; // DX12 SRV heap index (UINT_MAX = none)
} SamplerInfo;

typedef struct {
  std::wstring   msg;
  bool      bBold;  // true == red bkg; false == black bkg
  float     birthTime;
  float     expireTime;
  int       category;
  bool      bSentToRemote;
  DWORD     color;  // 0 = use default font color
} ErrorMsg;

typedef std::vector<ErrorMsg> ErrorMsgList;

class CShaderParams {
public:
  // float4 handles:
  D3DXHANDLE rand_frame;
  D3DXHANDLE rand_preset;
  D3DXHANDLE const_handles[24];
  D3DXHANDLE q_const_handles[(NUM_Q_VAR + 3) / 4];
  D3DXHANDLE rot_mat[24];

  typedef std::vector<TexSizeParamInfo> TexSizeParamInfoList;
  TexSizeParamInfoList texsize_params;

  // sampler stages for various PS texture bindings:
  //int texbind_vs;
  //int texbind_disk[32];
  //int texbind_voronoi;
  //...
  SamplerInfo   m_texture_bindings[32];  // an entry for each texture slot (t-register).  These are ALIASES - DO NOT DELETE.
  tex_code      m_texcode[32];  // if ==TEX_VS, forget the pointer - texture bound @ that stage is the double-buffered VS.

  void Clear();
  void CacheParams(LPD3DXCONSTANTTABLE pCT, bool bHardErrors);
  void OnTextureEvict(LPDIRECT3DBASETEXTURE9 texptr);
  CShaderParams();
  ~CShaderParams();
};

typedef std::vector<CShaderParams*> CShaderParamsList;

class VShaderInfo {
public:
  IDirect3DVertexShader9* ptr;
  LPD3DXCONSTANTTABLE     CT;
  CShaderParams           params;
  VShaderInfo() { ptr = NULL; CT = NULL; params.Clear(); }
  ~VShaderInfo() { Clear(); }
  void Clear();
};

class PShaderInfo {
public:
  IDirect3DPixelShader9* ptr;
  LPD3DXCONSTANTTABLE     CT;
  CShaderParams           params;
  LPD3DXBUFFER            bytecodeBlob;  // DX12: compiled SM5.0 bytecode for PSO creation
  PShaderInfo() { ptr = NULL; CT = NULL; bytecodeBlob = NULL; params.Clear(); }
  ~PShaderInfo() { Clear(); }
  void Clear();
};

typedef struct {
  VShaderInfo vs;
  PShaderInfo ps;
} ShaderPairInfo;

typedef struct {
  PShaderInfo warp;
  PShaderInfo comp;
  PShaderInfo bufferA;  // Shadertoy Buffer A (pre-comp pass)
  PShaderInfo bufferB;  // Shadertoy Buffer B (second compute buffer)
} PShaderSet;

typedef struct {
  VShaderInfo warp;
  VShaderInfo comp;
} VShaderSet;

typedef struct {
  std::wstring  szFilename;    // without path
  float    fRatingThis;
  float    fRatingCum;
} PresetInfo;
typedef std::vector<PresetInfo> PresetList;


class Engine : public EngineShell {
public:
  MDropDX12* mdropdx12;

  // Messages/Sprites mode helpers
  bool MessagesEnabled() const { return (m_nSpriteMessagesMode & 1) != 0; }
  bool SpritesEnabled()  const { return (m_nSpriteMessagesMode & 2) != 0; }

  //====[ 1. members added to create this specific example plugin: ]================================================

// =========================================================
// SPOUT variables
  spoutDX12 spoutsender;  // Spout DX12 sender (D3D11On12 interop)

  char WinampSenderName[256]; // The sender name
  bool bInitialized; // did it work ?

  // Wrapped DX12 backbuffers for Spout DX11 send
  ID3D11Resource* m_pWrappedBackBuffers[DXC_FRAME_COUNT] = {};
  bool m_bSpoutDX12Ready = false; // SpoutDX12 initialized and wraps valid

  bool OpenSender(unsigned int width, unsigned int height);
  void SpoutReleaseWraps(); // Release wrapped backbuffers and mark not ready
  void OpenMDropDX12Remote();
  void SetAudioDeviceDisplayName(const wchar_t* displayName, bool isRenderDevice);
  void ExecuteRenderCommand(const RenderCommand& cmd) override;
  int  GetPresetCount() override;
  int  GetCurrentPresetIndex() override;
  const wchar_t* GetPresetName(int idx) override;

  void SaveShaderBytecodeToFile(ID3DXBuffer* pShaderByteCode, uint32_t checksum, char* prefix);
  ID3DXBuffer* LoadShaderBytecodeFromFile(uint32_t checksum, char* prefix);

  uint32_t crc32(const char* data, size_t length);

  bool CheckDX9DLL();
  bool CheckForDirectX9c();
  void ShowDirectXMissingMessage();

  bool bSpoutChanged; // set to write config on exit

  bool bEnablePresetStartup;
  bool bAutoLockPresetWhenNoMusic;
  //bool StartupPresetLoaded = false;
  unsigned int g_Width;
  unsigned int g_Height;
  HWND g_hwnd;
  HDC g_hdc;
  wchar_t	m_szSavedSongTitle[512]; // for saving song tile with Spout on or off
  // =========================================================

  // =========================================================
  // DISPLAY OUTPUTS (monitor mirrors + Spout senders)
  std::vector<DisplayOutput> m_displayOutputs;
  ComPtr<ID3D12CommandAllocator>     m_mirrorCmdAllocators[DXC_FRAME_COUNT];
  ComPtr<ID3D12GraphicsCommandList>  m_mirrorCmdList;
  bool m_bMirrorClassRegistered = false;
  bool m_bMirrorsActive = false;       // Displays tab button; always starts off
  bool m_bMirrorModeForAltS = false;   // When true, ALT-S activates mirrors+fullscreen instead of stretch
  bool m_bMirrorPromptDisabled = false; // Skip "no mirrors enabled" prompt; auto-enable all
  std::atomic<bool> m_bMirrorStylesDirty{false}; // UI thread sets; render thread applies

  enum MirrorActivateResult { MirrorActivated, MirrorFullscreenOnly, MirrorCancelled };
  MirrorActivateResult TryActivateMirrors(HWND hRenderWnd);

  void EnumerateDisplayOutputs();
  void LoadDisplayOutputSettings();
  void SaveDisplayOutputSettings();
  void InitDisplayOutput(DisplayOutput& out);
  void DestroyDisplayOutput(DisplayOutput& out);
  void DestroyAllDisplayOutputs();
  void ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH);
  void SendToDisplayOutputs() override;
  void RefreshDisplaysTab();
  void ApplyMirrorWindowStyles();   // apply click-through + opacity to all active mirrors (render thread only)
  bool SaveDisplayProfile(const wchar_t* filePath);
  bool LoadDisplayProfile(const wchar_t* filePath);
  void UpdateDisplaysTabSelection(int sel);
  int  m_nDisplaysTabSel = -1;  // Selected index in Displays tab listbox

  // Configurable hotkeys (local + global)
  HotkeyBinding m_hotkeys[NUM_HOTKEYS];
  void ResetHotkeyDefaults();
  void LoadHotkeySettings();
  void SaveHotkeySettings();
  void RegisterGlobalHotkeys(HWND hwnd);
  void UnregisterGlobalHotkeys(HWND hwnd);
  bool DispatchHotkeyAction(int actionId);
  bool LookupLocalHotkey(UINT vk, UINT modifiers);
  bool DispatchHotkeyByTag(const std::wstring& tag);
  std::wstring FormatHotkeyDisplay(UINT modifiers, UINT vk);

  // Dynamic F1 help text (generated from binding table)
  wchar_t m_szHelpPage1[8192] = {};
  wchar_t m_szHelpPage2[8192] = {};
  void GenerateHelpText();

  // Dynamic user-added hotkeys (Script Commands and Launch Apps)
  std::vector<UserHotkey> m_userHotkeys;
  int m_nextUserHotkeyId = USER_HOTKEY_ID_BASE;
  int  AddUserHotkey(UserHotkeyType type);          // returns index in m_userHotkeys
  void RemoveUserHotkey(int index);
  void LaunchOrFocusApp(const std::wstring& path);

  // Idle timer (screensaver mode)
  bool m_bIdleTimerEnabled = false;
  int  m_nIdleTimeoutMinutes = 5;     // 1-60 minutes
  int  m_nIdleAction = 0;             // 0 = Fullscreen, 1 = Stretch/Mirror
  bool m_bIdleAutoRestore = true;     // True = restore on mouse/keyboard; false = manual hotkey only
  bool m_bIdleActivated = false;      // True when idle timer triggered activation
  void LoadIdleTimerSettings();
  void SaveIdleTimerSettings();
  // =========================================================


  /// CONFIG PANEL SETTINGS THAT WE'VE ADDED (TAB #2)
  bool		m_bFirstRun;
  bool    m_bSelfBootstrapped = false; // true when exe ran from empty directory (no resources found)
  float		m_fBlendTimeAuto;		// blend time when preset auto-switches
  float		m_fBlendTimeUser;		// blend time when user loads a new preset
  float		m_fTimeBetweenPresets;		// <- this is in addition to m_fBlendTimeAuto
  float		m_fTimeBetweenPresetsRand;	// <- this is in addition to m_fTimeBetweenPresets
  bool    m_bSequentialPresetOrder;
  bool		m_bHardCutsDisabled;
  float		m_fHardCutLoudnessThresh;
  int     m_nInjectEffectMode;   // 0=off 1=brighten 2=darken 3=solarize 4=invert (F11)
  float		m_fHardCutHalflife;
  float		m_fHardCutThresh;
  //int			m_nWidth;
  //int			m_nHeight;
  //int			m_nDispBits;
  int     m_nCanvasStretch;   // 0=Auto, 100=None, 125 = 1.25X, 133, 150, 167, 200, 300, 400 (4X).
  int			m_nTexSizeX;			// -1 = exact match to screen; -2 = nearest power of 2.
  int			m_nTexSizeY;
  float   m_fAspectX;
  float   m_fAspectY;
  float   m_fInvAspectX;
  float   m_fInvAspectY;
  int     m_nTexBitsPerCh;
  int			m_nGridX;
  int			m_nGridY;
  int			m_nMixType = -1; // -1 = Random

  // bool		m_bShowPressF1ForHelp;
  //char		m_szMonitorName[256];
  bool		m_bShowMenuToolTips;
  int			m_n16BitGamma;
  bool		m_bAutoGamma;
  //int		m_nFpsLimit;
  //int			m_cLeftEye3DColor[3];
  //int			m_cRightEye3DColor[3];
  bool		m_bEnableRating;
  //bool        m_bInstaScan;
  bool		m_bSongTitleAnims;
  int     m_nSpriteMessagesMode = 3;  // 0=Off, 1=Messages, 2=Sprites, 3=Messages & Sprites
  float		m_fSongTitleAnimDuration;
  float		m_fTimeBetweenRandomSongTitles;
  float		m_fTimeBetweenRandomCustomMsgs;
  int			m_nSongTitlesSpawned;
  int			m_nCustMsgsSpawned;
  bool    m_bEnablePresetStartup;
  bool    m_bEnableAudioCapture = true;
  float   m_fAudioSensitivity = -1.0f;  // -1 = adaptive auto-normalize (default); 0.5–256 = fixed gain multiplier
  bool    m_bEnablePresetStartupSavingOnClose = true;
  bool    m_bAutoLockPresetWhenNoMusic;
  bool    m_bScreenDependentRenderMode;
  int     m_nBassStart = 0;
  int     m_nBassEnd = 250;
  int     m_nMidStart = 250;
  int     m_nMidEnd = 4000;
  int     m_nTrebStart = 4000;
  int     m_nTrebEnd = 20000;
  float   m_MessageDefaultBurnTime = 0.1f;
  float   m_MessageDefaultFadeinTime = 0.2f;
  float   m_MessageDefaultFadeoutTime = 0.0f;
  
  bool m_WindowBorderless = false;
  float m_WindowWatermarkModeOpacity = 0.3f;
  int m_WindowX = 0;
  int m_WindowY = 0;
  int m_WindowWidth = 0;
  int m_WindowHeight = 0;
  int m_WindowFixedWidth = 960;
  int m_WindowFixedHeight = 540;
  
  // Preset mouse interaction controls
  bool m_bEnableMouseInteraction = true;
  float m_mouseX = 0.5;
  float m_mouseY = 0.5;
  float m_lastMouseX;
  float m_lastMouseY;
  bool m_mouseDown;
  int m_mouseClicked;

  // Shadertoy iMouse state (pixel coordinates, bottom-left origin)
  float m_stMouseX = 0.f;      // drag position x (pixels), persists when released
  float m_stMouseY = 0.f;      // drag position y (pixels), persists when released
  float m_stClickX = 0.f;      // click-start position x (pixels)
  float m_stClickY = 0.f;      // click-start position y (pixels)
  bool  m_stMouseDown = false;  // left button currently held
  bool  m_stMouseJustClicked = false; // true for one frame on click

  float fOpacity = 1.0f; // 0.0f = 100% transparent, 1.0f = 100% opaque
  bool m_RemotePresetLink = false;
  bool m_bAlwaysOnTop = false;

  enum TrackInfoSource { TRACK_SOURCE_SMTC = 0, TRACK_SOURCE_IPC = 1, TRACK_SOURCE_WINDOW = 2 };
  int m_nTrackInfoSource = TRACK_SOURCE_SMTC;
  bool m_bSongInfoOverlay = true;           // show overlay text notifications on track change
  wchar_t m_szTrackWindowTitle[256] = {};   // window title to scrape (TRACK_SOURCE_WINDOW) — legacy, migrated to profiles

  std::vector<WindowTitleProfile> m_windowTitleProfiles;
  int m_nActiveWindowTitleProfile = 0;

  bool m_SongInfoPollingEnabled = true;
  int m_SongInfoDisplayCorner = 3;

  bool m_ChangePresetWithSong = true;
  float m_SongInfoDisplaySeconds = 5.0f;
  bool m_bSongInfoAlwaysShow = false;
  bool m_DisplayCover = true;
  bool m_DisplayCoverWhenPressingB = true;
  float m_MediaKeyNotifyTime = 1.0f;  // seconds to show media key notification
  bool m_HideNotificationsWhenRemoteActive = false;

  // Error Display Settings — Normal mode
  float   m_ErrorDuration       = 8.0f;     // seconds
  int     m_ErrorFontSize       = 20;        // pixels (0 = auto)
  int     m_ErrorCorner         = 0;         // 0=UR, 1=UL, 2=LR, 3=LL
  int     m_ErrorColorR         = 255;
  int     m_ErrorColorG         = 255;
  int     m_ErrorColorB         = 255;
  wchar_t m_szErrorFontFace[128] = L"Segoe UI";

  // Error Display Settings — LOUD mode
  float   m_LoudDuration        = 30.0f;
  int     m_LoudFontSize        = 0;         // 0 = auto (window height / 6)
  int     m_LoudColorR1         = 255;
  int     m_LoudColorG1         = 50;
  int     m_LoudColorB1         = 50;
  int     m_LoudColorR2         = 255;
  int     m_LoudColorG2         = 255;
  int     m_LoudColorB2         = 50;
  int     m_LoudPulseSpeed      = 2;         // cycles per second

  int m_MinPSVersionConfig = 2;
  int m_MaxPSVersionConfig = 6;
  bool m_ShowUpArrowInDescriptionIfPSMinVersionForced = true;

  // GPU Protection Settings
  int  m_nMaxShapeInstances = 0;         // Cap per-shape instance count (0=unlimited, e.g. 512)
  bool m_bScaleInstancesByResolution = false; // Scale down num_inst at resolutions above base
  int  m_nInstanceScaleBaseWidth = 1920; // Reference width for instance scaling (instances scale down above this)
  bool m_bSkipHeavyPresets = false;      // Auto-skip presets exceeding GPU safety thresholds
  int  m_nHeavyPresetMaxInstances = 4096; // Total shape instances across all shapes that triggers skip

  //bool		m_bAlways3D;
  //float       m_fStereoSep;
  //bool		m_bAlwaysOnTop;
  //bool		m_bFixSlowText;
  //bool		m_bWarningsDisabled;		// messageboxes
  bool		    m_bWarningsDisabled2;		// warnings/errors in upper-right corner (m_szUserMessage)
  //bool        m_bAnisotropicFiltering;
  bool        m_bPresetLockOnAtStartup;
  bool        m_bPreventScollLockHandling;
  int         m_nMaxPSVersion_ConfigPanel;  // -1 = auto, 0 = disable shaders, 2 = ps_2_0, 3 = ps_3_0
  int         m_nMaxPSVersion_DX9;          // 0 = no shader support, 2 = ps_2_0, 3 = ps_3_0
  int         m_nMaxPSVersion;              // this one will be the ~min of the other two.  0/2/3.
  int         m_nMaxImages;
  int         m_nMaxBytes;

  HFONT       m_gdi_title_font_doublesize;
  LPD3DXFONT  m_d3dx_title_font_doublesize;

  // PIXEL SHADERS
  DWORD                   m_dwShaderFlags;       // Shader compilation/linking flags
  //ID3DXFragmentLinker*    m_pFragmentLinker;     // Fragment linker interface
  //LPD3DXBUFFER            m_pCompiledFragments;  // Buffer containing compiled fragments
  LPD3DXBUFFER            m_pShaderCompileErrors;
  VShaderSet              m_fallbackShaders_vs;  // *these are the only vertex shaders used for the whole app.*
  PShaderSet              m_fallbackShaders_ps;  // these are just used when the preset's pixel shaders fail to compile.
  PShaderSet              m_shaders;     // includes shader pointers and constant tables for warp & comp shaders, for cur. preset
  PShaderSet              m_OldShaders;  // includes shader pointers and constant tables for warp & comp shaders, for prev. preset
  PShaderSet              m_NewShaders;  // includes shader pointers and constant tables for warp & comp shaders, for coming preset
  ShaderPairInfo          m_BlurShaders[2];
  bool                    m_bWarpShaderLock;
  bool                    m_bCompShaderLock;
  //bool LoadShaderFromFile( char* szFile, char* szFn, char* szProfile,
  //                         LPD3DXCONSTANTTABLE* ppConstTable, void** ppShader );
#define SHADER_WARP  0
#define SHADER_COMP  1
#define SHADER_BLUR  2
#define SHADER_OTHER 3
  bool LoadShaderFromMemory(const char* szShaderText, char* szFn, char* szProfile,
    LPD3DXCONSTANTTABLE* ppConstTable, void** ppShader, int shaderType, bool bHardErrors, bool compileOnly,
    LPD3DXBUFFER* ppBytecodeOut = nullptr, const char* szDiagName = nullptr);
  bool RecompileVShader(const char* szShadersText, VShaderInfo* si, int shaderType, bool bHardErrors, bool bCompileOnly);
  bool RecompilePShader(const char* szShadersText, PShaderInfo* si, int shaderType, bool bHardErrors, int PSVersion, bool bCompileOnly, const char* szDiagName = nullptr);
  bool EvictSomeTexture();
  typedef std::vector<TexInfo> TexInfoList;
  TexInfoList     m_textures;
  bool m_bNeedRescanTexturesDir;
  // vertex declarations:
  IDirect3DVertexDeclaration9* m_pSpriteVertDecl;
  IDirect3DVertexDeclaration9* m_pWfVertDecl;
  IDirect3DVertexDeclaration9* m_pMyVertDecl;

  D3DXVECTOR4 m_rand_frame;  // 4 random floats (0..1); randomized once per frame; fed to pixel shaders.

  // RUNTIME SETTINGS THAT WE'VE ADDED
  float   m_prev_time;
  bool    m_bTexSizeWasAutoPow2;
  bool    m_bTexSizeWasAutoExact;
  bool    m_bPresetLockedByUser;
  bool    m_bPresetLockedByCode;
  bool    m_ShaderCaching = true;
  bool    m_ShaderPrecompileOnStartup = true;
  bool    m_CheckDirectXOnStartup = true;
  int     m_LogLevel = 1; // 0=Off, 1=Error, 2=Warn, 3=Info, 4=Verbose
  int     m_LogOutput = 3; // LOG_OUTPUT_BOTH (FILE|ODS), see utility.h
  bool    m_ShowLockSymbol = true;
  float   m_fAnimTime;
  float   m_fStartTime;
  float   m_fPresetStartTime;
  bool    m_bPresetDiagLogged = false;
  float   m_fNextPresetTime;
  float   m_fSnapPoint;
  CState* m_pState;				// points to current CState
  CState* m_pOldState;			// points to previous CState
  CState* m_pNewState;			// points to the coming CState - we're not yet blending to it b/c we're still compiling the shaders for it!
  int     m_nLoadingPreset;
  wchar_t m_szLoadingPreset[MAX_PATH];
  float   m_fLoadingPresetBlendTime;
  std::thread        m_presetLoadThread;      // background thread for async shader compilation
  std::atomic<bool>  m_bPresetLoadReady{false}; // set by bg thread when Import + shaders are done
  std::atomic<uint64_t> m_nLoadGeneration{0}; // incremented each load; bg thread checks before signaling
  float   m_fLoadStartTime = 0;              // GetTime() when async load began (for timeout)
  float   m_fShaderCompileTimeout = 8.0f;    // seconds before auto-skipping a stuck compilation
  bool    m_bLoadingShadertoyMode = false;    // true when async load is for a .milk3 Shadertoy preset
  int     m_nPresetsLoadedTotal; //important for texture eviction age-tracking...
  CState	m_state_DO_NOT_USE[3];	// do not use; use pState and pOldState instead.
  ui_mode	m_UI_mode;				// can be UI_REGULAR, UI_LOAD, UI_SAVEHOW, or UI_SAVEAS

#define MASH_SLOTS 5
#define MASH_APPLY_DELAY_FRAMES 1
  int         m_nMashSlot;    //0..MASH_SLOTS-1
  int         m_nMashPreset[MASH_SLOTS];
  int         m_nLastMashChangeFrame[MASH_SLOTS];

  bool		m_bUserPagedUp;
  bool		m_bUserPagedDown;
  float		m_fMotionVectorsTempDx;
  float		m_fMotionVectorsTempDy;

  td_waitstr  m_waitstring;
  void		WaitString_NukeSelection();
  void		WaitString_Cut();
  void		WaitString_Copy();
  void		WaitString_Paste();
  void		WaitString_SeekLeftWord();
  void		WaitString_SeekRightWord();
  int			WaitString_GetCursorColumn();
  int			WaitString_GetLineLength();
  void		WaitString_SeekUpOneLine();
  void		WaitString_SeekDownOneLine();

  int			m_nPresets;			// the # of entries in the file listing.  Includes directories and then files, sorted alphabetically.
  int			m_nDirs;			// the # of presets that are actually directories.  Always between 0 and m_nPresets.
  int			m_nPresetFilter = 0;	// 0=all, 1=.milk only, 2=.milk2 only, 3=.milk3 only
  int			m_nPresetListCurPos;// Index of the currently-HIGHLIGHTED preset (the user must press Enter on it to select it).
  int			m_nCurrentPreset;	// Index of the currently-RUNNING preset.
  //   Note that this is NOT the same as the currently-highlighted preset! (that's m_nPresetListCurPos)
  //   Be careful - this can be -1 if the user changed dir. & a new preset hasn't been loaded yet.
  wchar_t		m_szCurrentPresetFile[512];	// w/o path.  this is always valid (unless no presets were found)
  PresetList  m_presets;
  void		UpdatePresetList(bool bBackground = false, bool bForce = false, bool bTryReselectCurrentPreset = true);
  wchar_t     m_szUpdatePresetMask[MAX_PATH];
  bool        m_bPresetListReady;
  //void		UpdatePresetRatings();
    //int         m_nRatingReadProgress;  // equals 'm_nPresets' if all ratings are read in & ready to go; -1 if uninitialized; otherwise, it's still reading them in, and range is: [0 .. m_nPresets-1]
  bool        m_bInitialPresetSelected;

  // PRESET HISTORY
#define PRESET_HIST_LEN (64+2)     // make this 2 more than the # you REALLY want to be able to go back.
  std::wstring m_presetHistory[PRESET_HIST_LEN];   //circular
  int m_presetHistoryPos;
  int m_presetHistoryBackFence;
  int m_presetHistoryFwdFence;
  void PrevPreset(float fBlendTime);
  void NextPreset(float fBlendTime);  // if not retracing our former steps, it will choose a random one.
  void OnFinishedLoadingPreset();
  int SendMessageToMDropDX12Remote(const wchar_t* presetFile);
  int SendMessageToMDropDX12Remote(const wchar_t* presetFile, bool doForce);
  void PostMessageToMDropDX12Remote(UINT msg);

#define WM_USER_NEXT_PRESET WM_USER + 100
#define WM_USER_PREV_PRESET WM_USER + 101
#define WM_USER_COVER_CHANGED WM_USER + 102
#define WM_USER_SPRITE_MODE WM_USER + 103
#define WM_USER_MESSAGE_MODE WM_USER + 104

  FFT            myfft;
  td_mysounddata mysound;

  // stuff for displaying text to user:
  bool		m_bShowFPS;
  bool		m_bShowRating;
  bool		m_bShowPresetInfo;
  bool		m_bShowDebugInfo;
  bool		m_bShowSongTitle;
  bool		m_bShowSongTime;
  bool		m_bShowSongLen;
  float		m_fShowRatingUntilThisTime;

#define ERR_ALL    0
#define ERR_INIT   1  //specifically, loading a preset
#define ERR_PRESET 2  //specifically, loading a preset
#define ERR_MISC   3
#define ERR_NOTIFY 4  // a simple notification - not an error at all. ("shuffle is now ON." etc.)
  // NOTE: each NOTIFY msg clears all the old NOTIFY messages!
#define ERR_SCANNING_PRESETS 5
#define ERR_MSG_BOTTOM_EXTRA_1 6
#define ERR_MSG_BOTTOM_EXTRA_2 7
#define ERR_MSG_BOTTOM_EXTRA_3 8


  ErrorMsgList m_errors;
  void SetFPSCap(int fps);

  // Script engine
  ScriptState m_script;
  void UpdateScript() override;
  void LoadScript(const wchar_t* path);
  void StartScript();
  void StopScript();
  void ExecuteScriptLine(int lineIndex);
  void ExecuteScriptLine(const wchar_t* text); // pipe-split + execute
  void ExecuteScriptCommand(const std::wstring& cmd);
  void SyncScriptUI();

  void AddNotification(wchar_t* szMsg);
  void AddNotificationAudioDevice();
  void AddNotification(wchar_t* szMsg, float time);
  void AddNotificationColored(wchar_t* szMsg, float time, DWORD color);
  void AddError(wchar_t* szMsg, float fDuration, int category = ERR_ALL, bool bBold = true);
  void AddLoudError(wchar_t* szMsg);
  void ClearErrors(int category = ERR_ALL);  // 0=all categories


  void GetSongTitle(wchar_t* szSongTitle, int nSize);

  //musik::core::sdk::IPlaybackService* playbackService;
  std::string emulatedWinampSongTitle;
  char		m_szDebugMessage[512];
  wchar_t		m_szSongTitle[512];
  wchar_t		m_szSongTitlePrev[512];

  // stuff for menu system:
  CMilkMenu* m_pCurMenu;	// should always be valid!
  CMilkMenu	 m_menuPreset;
  CMilkMenu	  m_menuWave;
  CMilkMenu	  m_menuAugment;
  CMilkMenu	  m_menuCustomWave;
  CMilkMenu	  m_menuCustomShape;
  CMilkMenu	  m_menuMotion;
  CMilkMenu	  m_menuPost;
  CMilkMenu    m_menuWavecode[MAX_CUSTOM_WAVES];
  CMilkMenu    m_menuShapecode[MAX_CUSTOM_SHAPES];
  bool         m_bShowShaderHelp;

  wchar_t		m_szMilkdrop2Path[MAX_PATH];		// ends in a backslash
  wchar_t		m_szMsgIniFile[MAX_PATH];
  wchar_t     m_szImgIniFile[MAX_PATH];
  wchar_t		m_szPresetDir[MAX_PATH];
  wchar_t     m_szPresetStartup[MAX_PATH];
  wchar_t     m_szAudioDevicePrevious[MAX_PATH];
  wchar_t     m_szAudioDevice[MAX_PATH];
  wchar_t     m_szAudioDeviceDisplayName[MAX_PATH];
  wchar_t     m_SongInfoFormat[MAX_PATH];
  wchar_t     m_szWindowTitle[256];         // configurable window title (empty = "MDropDX12 Visualizer")
  wchar_t     m_szRemoteWindowTitle[256];   // configurable remote title (empty = "MDropDX12 Remote")
  wchar_t     m_szLastRemoteExePath[MAX_PATH] = {};  // last pipe-connected Remote exe path (for launch)
  int m_nSettingsCurSel = 0;       // currently highlighted setting in UI_SETTINGS
  bool m_bSettingsNeedAttention = false; // force settings open on bad config
  int m_nAudioLoopState = 0; // 0: Running, 1: Cancel running thread, 2: Must restart
  int m_nAudioDeviceRequestType = 0; // 0: Undefined, 1: Capture (in), 2: Render (out)
  int m_nAudioDeviceActiveType = 2;   // 0: Unknown, 1: Capture (in), 2: Render (out)
  int m_nAudioDevicePreviousType = 2;
  float		m_fRandStart[4];

  // DIRECTX 9 (legacy — kept for compilation; always nullptr at runtime):
  IDirect3DTexture9* m_lpVS[2];
#define NUM_BLUR_TEX 6
#if (NUM_BLUR_TEX>0)
  IDirect3DTexture9* m_lpBlur[NUM_BLUR_TEX]; // each is successively 1/2 size of prev.
  int               m_nBlurTexW[NUM_BLUR_TEX];
  int               m_nBlurTexH[NUM_BLUR_TEX];
#endif
  int m_nHighestBlurTexUsedThisFrame;

#define NUM_SUPERTEXTS 10
  IDirect3DTexture9* m_lpDDSTitle[NUM_SUPERTEXTS];
  td_supertext m_supertexts[NUM_SUPERTEXTS];

  // DX12 render targets (Phase 2)
  DX12Texture m_dx12VS[2];                    // double-buffered visualizer canvas
  DX12Texture m_dx12Blur[NUM_BLUR_TEX];       // blur pyramid (6 levels)
  DX12Texture m_dx12Title[NUM_SUPERTEXTS];    // title overlays
  ComPtr<ID3D12Resource> m_dx12TitleUploadBuf[NUM_SUPERTEXTS]; // per-slot upload buffers (avoids cross-slot corruption)
  HDC         m_titleDC = nullptr;             // GDI memory DC for title text rendering
  HBITMAP     m_titleDIB = nullptr;            // DIB section for title text
  BYTE*       m_titleDIBBits = nullptr;        // pixel data pointer

  // DX12 preset PSOs (Phase 5)
  ComPtr<ID3D12PipelineState> m_dx12WarpPSO;         // current preset warp
  ComPtr<ID3D12PipelineState> m_dx12CompPSO;         // current preset comp
  ComPtr<ID3D12PipelineState> m_dx12FallbackWarpPSO; // default warp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12FallbackCompPSO; // default comp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12BlurPSO[2];      // [0] = horiz (blur1), [1] = vert (blur2)
  DX12Texture m_injectEffectTex;                     // back-buffer-sized copy for F11 inject post-process
  ComPtr<ID3D12PipelineState> m_pInjectEffectPSO;    // inject effect pixel shader PSO
  void RenderInjectEffect();                         // F11 inject effect post-process pass
  DX12Texture m_dx12Feedback[2];                      // ping-pong feedback buffers for Buffer A (FLOAT32)
  DX12Texture m_dx12ImageFeedback[2];                 // ping-pong feedback buffers for Image pass (FLOAT32)
  int m_nFeedbackIdx = 0;                            // read index (write = 1 - read), shared by both pairs
  bool m_bCompUsesFeedback = false;                  // true when comp shader uses sampler_feedback
  bool m_bCompUsesImageFeedback = false;             // true when comp shader uses sampler_image

  // Audio FFT/waveform texture for Shadertoy shaders (512x2 R32_FLOAT)
  // Row 0 = FFT spectrum (512 bins, 0-11kHz), Row 1 = PCM waveform (512 samples)
  DX12Texture m_dx12AudioTex;
  Microsoft::WRL::ComPtr<ID3D12Resource> m_audioUploadBuffer;
  void CreateAudioTexture();
  void UpdateAudioTexture();   // per-frame: upload latest FFT/waveform to GPU

  // Custom channel textures from Shader Import (user-selected texture files)
  DX12Texture m_dx12ChannelTex[4];           // loaded textures for sampler_chtex0..3
  std::wstring m_szChannelTexPath[4];        // file paths (set by Import UI)
  bool m_bHasBufferA = false;                        // true when preset has a Buffer A shader
  bool m_bHasBufferB = false;                        // true when preset has a Buffer B shader
  bool m_bShadertoyMode = false;                     // true when a .milk3 Shadertoy preset is active
  int  m_nShadertoyStartFrame = 0;                   // frame at which Shadertoy mode was activated (for iFrame=0)
  ComPtr<ID3D12PipelineState> m_dx12BufferAPSO;      // Buffer A pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferBPSO;      // Buffer B pixel shader PSO
  DX12Texture m_dx12FeedbackB[2];                    // ping-pong feedback buffers for Buffer B (FLOAT32)
  std::atomic<int> m_nRecompileResult{0};            // 0=idle, 1=pending, 2=done-ok, 3=done-fail
  void CopyBackbufferToFeedback();                   // capture comp output for next frame's feedback (single-pass)
  void RenderFrameShadertoy(ID3D12GraphicsCommandList* cmdList);  // Shadertoy pipeline (skip warp/blur/shapes)
  UINT m_warpMainTexSlot = 0;                         // t-register for sampler_main in warp PS
  UINT m_compMainTexSlot = 0;                         // t-register for sampler_main in comp PS
  bool m_bDX12PSOsDirty = false;                      // deferred PSO creation flag
  void CreateDX12PresetPSOs();                        // creates PSOs from m_shaders bytecodes
  void DX12_BlurPasses();                             // DX12 implementation of BlurPasses()

  // ── Video Input (Spout / Webcam / Video File) ──
  enum VideoInputSource {
      VID_SOURCE_NONE   = 0,
      VID_SOURCE_SPOUT  = 1,
      VID_SOURCE_WEBCAM = 2,
      VID_SOURCE_FILE   = 3
  };
  int     m_nVideoInputSource = VID_SOURCE_NONE; // active source type

  // Webcam / Video File capture (Media Foundation)
  std::unique_ptr<class VideoCaptureSource> m_videoCapture;
  wchar_t m_szWebcamDevice[256] = {};   // friendly name of selected webcam
  wchar_t m_szVideoFile[MAX_PATH] = {}; // path to video file
  bool    m_bVideoLoop = true;          // loop video file playback

  void    InitVideoCapture();
  void    DestroyVideoCapture();
  void    UpdateVideoCaptureTexture();   // per-frame GPU upload

  // Spout receiver (source type 1)
  struct SpoutInputState {
      spoutDX12 receiver;
      ComPtr<ID3D12Resource> pReceivedTexture;
      DX12Texture dx12InputTex;
      UINT nSenderWidth = 0, nSenderHeight = 0;
      bool bReceiverReady = false;
      bool bConnected = false;
  };
  std::unique_ptr<SpoutInputState> m_spoutInput;

  // Shared video input settings (apply to all sources)
  bool    m_bSpoutInputEnabled = false;  // kept for backward compat (maps to m_nVideoInputSource != 0)
  bool    m_bSpoutInputOnTop = false;       // false=background, true=overlay
  float   m_fSpoutInputOpacity = 1.0f;
  bool    m_bSpoutInputLumaKey = false;
  float   m_fSpoutInputLumaThreshold = 0.1f;
  float   m_fSpoutInputLumaSoftness = 0.1f;
  wchar_t m_szSpoutInputSender[256] = {};
  ComPtr<ID3D12PipelineState> m_pSpoutInputPSO;

  void InitSpoutInput();
  void DestroySpoutInput();
  void UpdateSpoutInputTexture();
  void CompositeSpoutInput(bool isBackground);
  void CompositeVideoInput(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH);
  void CompileSpoutInputPSO();
  void EnumerateSpoutSenders(std::vector<std::string>& outNames);
  void SaveSpoutInputSettings();
  void LoadSpoutInputSettings();

  // ── Video Effects ──
  struct AudioLink {
      int   source    = 0;    // 0=none, 1=bass, 2=mid, 3=treb, 4=vol
      float intensity = 0.5f; // 0.0–2.0
  };
  struct VideoEffectParams {
      // Transform
      float posX = 0, posY = 0;       // -1 to 1
      float scale = 1.0f;             // 0.1 to 5.0
      float rotation = 0;             // 0–360 degrees
      bool  mirrorH = false, mirrorV = false;
      // Color
      float tintR = 1, tintG = 1, tintB = 1; // 0–2
      float brightness = 0;           // -1 to 1
      float contrast = 1.0f;          // 0–3
      float saturation = 1.0f;        // 0–3
      float hueShift = 0;             // 0–360
      bool  invert = false;
      // Effects
      float pixelation = 0;           // 0 (off) to 1 (max)
      float chromatic = 0;            // 0 (off) to 0.05
      bool  edgeDetect = false;
      // Blend: 0=Alpha, 1=Additive, 2=Multiply, 3=Screen, 4=Overlay, 5=Difference
      int   blendMode = 0;
      // Audio-reactive links
      AudioLink arPosX, arPosY, arScale, arRotation;
      AudioLink arBrightness, arSaturation, arChromatic;

      bool IsDefault() const {
          return posX == 0 && posY == 0 && scale == 1.0f && rotation == 0
              && !mirrorH && !mirrorV
              && tintR == 1 && tintG == 1 && tintB == 1
              && brightness == 0 && contrast == 1.0f && saturation == 1.0f
              && hueShift == 0 && !invert
              && pixelation == 0 && chromatic == 0 && !edgeDetect
              && blendMode == 0
              && arPosX.source == 0 && arPosY.source == 0
              && arScale.source == 0 && arRotation.source == 0
              && arBrightness.source == 0 && arSaturation.source == 0
              && arChromatic.source == 0;
      }
  };
  VideoEffectParams m_videoFX;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Alpha;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Additive;
  ComPtr<ID3D12PipelineState> m_pVideoFX_PSO_Solid;   // for shader-based blend modes 2-5
  DX12Texture m_dx12VideoFXDest;                       // RT copy for shader-based blends
  void CompileVideoFXPSOs();
  void CompositeVideoInputFX(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH);

  // Video Effects Window
  class VideoEffectsWindow* m_pVideoEffectsWindow = nullptr;
  void OpenVideoEffectsWindow();
  void CloseVideoEffectsWindow();

  // Video FX Profiles
  wchar_t m_szCurrentVFXProfile[MAX_PATH] = {};  // currently loaded profile (empty = none)
  bool    m_bEnableVFXStartup = false;
  wchar_t m_szVFXStartup[MAX_PATH] = {};
  bool    m_bEnableVFXStartupSavingOnClose = true;
  void    SaveVideoFXProfile(const wchar_t* path);
  bool    LoadVideoFXProfile(const wchar_t* path);
  void    GetVideoFXProfileDir(wchar_t* out, size_t len);

  // VFX Profile Picker Window
  class VFXProfileWindow* m_pVFXProfileWindow = nullptr;
  void OpenVFXProfileWindow();
  void CloseVFXProfileWindow();

  // ── Game Controller ──
  bool    m_bControllerEnabled = false;
  int     m_nControllerDeviceID = -1;    // winmm joy ID (0-15), -1 = none
  wchar_t m_szControllerName[256] = {};  // friendly name for INI persistence
  DWORD   m_dwLastControllerButtons = 0; // previous frame's button state
  std::map<int, std::string> m_controllerConfig; // button# → command
  std::string m_szControllerJSONText;    // raw JSON text for UI edit control

  void PollController();
  void ExecuteControllerCommand(const std::string& cmd);
  void EnumerateControllers(HWND hCombo);
  void LoadControllerJSON();
  void SaveControllerJSON(const std::string& jsonText);
  void LoadControllerSettings();
  void SaveControllerSettings();
  std::string GetDefaultControllerJSON();
  void ParseControllerJSON(const std::string& jsonText);
  void ShowControllerHelpPopup(HWND hParent);

  // ── MIDI ──
  bool    m_bMidiEnabled = false;
  int     m_nMidiDeviceID = -1;        // winmm MIDI input device ID
  wchar_t m_szMidiDeviceName[256] = {};
  int     m_nMidiBufferDelay = 30;     // CC debounce delay (ms)
  std::vector<MidiRow> m_midiRows;     // 50 mapping slots
  MidiInput m_midiInput;

  void LoadMidiJSON();
  void SaveMidiJSON();
  void LoadMidiSettings();
  void SaveMidiSettings();
  void ParseMidiJSON(const std::string& json);
  std::string SerializeMidiJSON() const;
  void ExecuteMidiButton(const MidiRow& row);
  void ExecuteMidiKnob(const MidiRow& row, int midiValue);
  void LoadMidiDefaultActions(std::vector<std::string>& out);
  void OpenMidiDevice();
  void CloseMidiDevice();

  int               m_nTitleTexSizeX, m_nTitleTexSizeY;
  UINT              m_adapterId;
  MYVERTEX* m_verts;
  MYVERTEX* m_verts_temp;
  td_vertinfo* m_vertinfo;
  int* m_indices_strip;
  int* m_indices_list;

  // for final composite grid:
#define FCGSX 32 // final composite gridsize - # verts - should be EVEN.
#define FCGSY 24 // final composite gridsize - # verts - should be EVEN.
                 // # of grid *cells* is two less,
                 // since we have redundant verts along the center line in X and Y (...for clean 'ang' interp)
  MYVERTEX    m_comp_verts[FCGSX * FCGSY];
  int         m_comp_indices[(FCGSX - 2) * (FCGSY - 2) * 2 * 3];

  bool		m_bMMX;
  //bool		m_bSSE;
  bool        m_bHasFocus;
  bool        m_bHadFocus;
  bool		m_bOrigScrollLockState;
  //bool      m_bMilkdropScrollLockState;  // saved when focus is lost; restored when focus is regained

  int         m_nNumericInputMode;	// NUMERIC_INPUT_MODE_CUST_MSG, NUMERIC_INPUT_MODE_SPRITE
  int         m_nNumericInputNum;
  int			m_nNumericInputDigits;
  td_custom_msg_font   m_CustomMessageFont[MAX_CUSTOM_MESSAGE_FONTS];
  td_custom_msg        m_CustomMessage[MAX_CUSTOM_MESSAGES];

  // Animation profiles
  td_anim_profile      m_AnimProfiles[MAX_ANIM_PROFILES];
  int                  m_nAnimProfileCount = 0;
  int                  m_nSongTitleAnimProfile = -1;   // -1 = default hardcoded, -2 = random, 0+ = profile
  int                  m_nPresetNameAnimProfile = -1;  // -1 = disabled, -2 = random, 0+ = profile

  texmgr      m_texmgr;		// for user sprites
  
  bool m_blackmode = false;

  IDirect3DTexture9* m_tracer_tex;

  int         m_nFramesSinceResize;

  char        m_szShaderIncludeText[32768];     // note: this still has char 13's and 10's in it - it's never edited on screen or loaded/saved with a preset.
  int         m_nShaderIncludeTextLen;          //  # of chars, not including the final NULL.
  char        m_szDefaultWarpVShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultWarpPShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultCompVShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szDefaultCompPShaderText[32768]; // THIS HAS CHAR 13/10 CONVERTED TO LINEFEED_CONTROL_CHAR
  char        m_szBlurVS[32768];
  char        m_szBlurPSX[32768];
  char        m_szBlurPSY[32768];
  void        GenWarpPShaderText(char* szShaderText, float decay, bool bWrap);
  void        GenCompPShaderText(char* szShaderText, float brightness, float ve_alpha, float ve_zoom, int ve_orient, float hue_shader, bool bBrighten, bool bDarken, bool bSolarize, bool bInvert);

  //====[ 2. methods added: ]=====================================================================================

  void RenderFrame(int bRedraw);
  void DX12_RenderWarpAndComposite();
  void DX12_DrawWave(float* fL, float* fR);
  void DX12_DrawSprites();
  void DX12_DrawCustomShapes();
  void DX12_DrawCustomWaves();
  void AlignWave(int nSamples);

  void        DrawTooltip(wchar_t* str, int xR, int yB);
  void        RandomizeBlendPattern();
  void        GenPlasma(int x0, int x1, int y0, int y1, float dt);
  void        CompilePresetShadersToFile(wchar_t* m_szCurrentPresetFile);
  void        ClearPreset();
  void        RemoveAngleBrackets(wchar_t* str);
  void        LoadPerFrameEvallibVars(CState* pState);
  void        LoadCustomWavePerFrameEvallibVars(CState* pState, int i);
  void        LoadCustomShapePerFrameEvallibVars(CState* pState, int i, int instance);
  void        WriteRealtimeConfig();	// called on Finish()
  void        dumpmsg(wchar_t* s, int level = LOG_INFO);
  void        Randomize();
  void        LoadRandomPreset(float fBlendTime);
  void        LoadPreset(const wchar_t* szPresetFilename, float fBlendTime);
  bool        ParseMilk2File(const wchar_t* szPath, wchar_t* outTemp1, wchar_t* outTemp2, int& outMixType, float& outProgress, int& outDirection);
  void        LoadMilk2Preset(const wchar_t* szPresetFilename, float fBlendTime);
  void        LoadMilk3Preset(const wchar_t* szPresetFilename, float fBlendTime);
  void        LoadPresetTick();
  bool        WaitForPendingLoad(DWORD timeoutMs = 3000); // waits for bg thread, applies via LoadPresetTick
  void        FindValidPresetDir();
  wchar_t* GetMsgIniFile() { return m_szMsgIniFile; };
  wchar_t* GetPresetDir() { return m_szPresetDir; };
  void		SavePresetAs(wchar_t* szNewFile);		// overwrites the file if it was already there.
  void		DeletePresetFile(wchar_t* szDelFile);
  void		RenamePresetFile(wchar_t* szOldFile, wchar_t* szNewFile);
  void		SetCurrentPresetRating(float fNewRating);
  void		SeekToPreset(wchar_t cStartChar);
  bool		ReversePropagatePoint(float fx, float fy, float* fx2, float* fy2);
  int 		HandleRegularKey(WPARAM wParam);
  void    SaveCurrentPresetToQuicksave(bool altDir);
  void		LaunchCustomMessage(int nMsgNum);
  void		LaunchMessage(wchar_t* sMessage);
  void    SendPresetChangedInfoToMDropDX12Remote();
  void    SendPresetWaveInfoToMDropDX12Remote();
  void    SendSettingsInfoToMDropDX12Remote();
  void    SetWaveParamsFromMessage(std::wstring& message);
  void		ReadCustomMessages();
  void		LaunchSongTitleAnim(int supertextIndex);
  void    PushSongTitleAsMessage();
  void    ApplyAnimProfileToSupertext(td_supertext& st, const td_anim_profile& prof);
  int     PickRandomAnimProfile();
  void    ReadAnimProfiles();
  void    WriteAnimProfiles();
  void    ExportAnimProfiles(wchar_t* szPath);
  void    ImportAnimProfiles(wchar_t* szPath);
  void    CreateDefaultAnimProfiles();
  void    CaptureScreenshot();
  bool    CaptureScreenshotWithFilename(wchar_t* outFilename, size_t outFilenameSize);

  bool		RenderStringToTitleTexture(int supertextIndex);
  void		ShowSongTitleAnim(/*IDirect3DTexture9* lpRenderTarget,*/ int w, int h, float fProgress, int supertextIndex);
  void		DrawWave(float* fL, float* fR);
  void        DrawCustomWaves();
  void        DrawCustomShapes();
  void		DrawSprites();
  void        ComputeGridAlphaValues();
  //void        WarpedBlit();
               // note: 'bFlipAlpha' just flips the alpha blending in fixed-fn pipeline - not the values for culling tiles.
  void		 WarpedBlit_Shaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling);
  void		 WarpedBlit_NoShaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling);
  void		 ShowToUser_Shaders(int nPass, bool bAlphaBlend, bool bFlipAlpha, bool bCullTiles, bool bFlipCulling);
  void		 ShowToUser_NoShaders();
  void        BlurPasses();
  void        GetSafeBlurMinMax(CState* pState, float* blur_min, float* blur_max);
  void		RunPerFrameEquations(int code);
  void		DrawUserSprites(int targetLayer = -1);  // -1 = all, 0 = behind text, 1 = on top
  void    DrawOnTopSprites() override { if (SpritesEnabled()) DrawUserSprites(1); }
  // SendToDisplayOutputs is declared with the display output members above
  void		MergeSortPresets(int left, int right);
  void		BuildMenus();
  void        SetMenusForPresetVersion(int WarpPSVersion, int CompPSVersion);
  // Settings screen (overlay)
  void        GetSettingValueString(int id, wchar_t* buf, int bufLen);
  const wchar_t* GetSettingHint(int id);
  void        ToggleSetting(int id);
  void        AdjustSetting(int id, int direction);
  void        SaveSettingToINI(int id);
  void        OpenFolderPickerForPresetDir(HWND hOwnerOverride = NULL);
  // Settings window (ToolWindow subclass, own thread)
  std::unique_ptr<SettingsWindow> m_settingsWindow;
  int         m_nSettingsFontSize = -16;     // Shared font size for all tool windows (negative = pixel height)
  void        OpenSettingsWindow();
  void        CloseSettingsWindow();

  // Spout / Displays window (ToolWindow subclass, own thread)
  std::unique_ptr<DisplaysWindow> m_displaysWindow;
  void OpenDisplaysWindow();
  void CloseDisplaysWindow();

  // Song Info window (ToolWindow subclass, own thread)
  std::unique_ptr<SongInfoWindow> m_songInfoWindow;
  void OpenSongInfoWindow();
  void CloseSongInfoWindow();

  // Hotkeys window (ToolWindow subclass, own thread)
  std::unique_ptr<HotkeysWindow> m_hotkeysWindow;
  void OpenHotkeysWindow();
  void CloseHotkeysWindow();

  // MIDI window (ToolWindow subclass, own thread)
  std::unique_ptr<MidiWindow> m_midiWindow;
  void OpenMidiWindow();
  void CloseMidiWindow();

  // Presets window (ToolWindow subclass, own thread)
  std::unique_ptr<PresetsWindow> m_presetsWindow;
  void OpenPresetsWindow();
  void ClosePresetsWindow();

  // Sprites window (ToolWindow subclass, own thread)
  std::unique_ptr<SpritesWindow> m_spritesWindow;
  void OpenSpritesWindow();
  void CloseSpritesWindow();

  // Messages window (ToolWindow subclass, own thread)
  std::unique_ptr<MessagesWindow> m_messagesWindow;
  void OpenMessagesWindow();
  void CloseMessagesWindow();

  // Button Board window (ToolWindow subclass, own thread)
  std::unique_ptr<ButtonBoardWindow> m_boardWindow;
  void OpenBoardWindow();
  void CloseBoardWindow();

  // Shader Import window (ToolWindow subclass, own thread)
  std::unique_ptr<ShaderImportWindow> m_shaderImportWindow;
  void OpenShaderImportWindow();
  void CloseShaderImportWindow();

  // Welcome window (no-presets prompt)
  std::unique_ptr<WelcomeWindow> m_welcomeWindow;
  void OpenWelcomeWindow();
  void CloseWelcomeWindow();

  // Text Animation Editor window (ToolWindow subclass, own thread)
  std::unique_ptr<TextAnimWindow> m_textAnimWindow;
  void OpenTextAnimWindow();
  void CloseTextAnimWindow();

  // Workspace Layout window
  std::unique_ptr<WorkspaceLayoutWindow> m_workspaceLayoutWindow;
  void OpenWorkspaceLayoutWindow();
  void CloseWorkspaceLayoutWindow();

  // Broadcast WM_MW_REBUILD_FONTS to all windows except the sender
  void BroadcastFontSync(HWND hSender);

  // Messages tab
  bool        ShowMsgOverridesDialog(HWND hParent);
  void        PopulateMsgListBox(HWND hList);
  void        BuildMsgPlaybackOrder();
  void        WriteCustomMessages();
  void        SaveMsgAutoplaySettings();
  void        LoadMsgAutoplaySettings();
  void        ScheduleNextAutoMessage();
  void        UpdateMsgPreview(HWND hSettingsWnd, int sel);
  bool        ShowMessageEditDialog(HWND hParent, int msgIndex, bool isNew);

  // Window Title Parser popup
  void        OpenWindowTitleParserPopup(HWND hParent);

  // Sprites tab (page 6)
  struct SpriteEntry {
    int          nIndex;           // [imgNN] number (0-99999)
    wchar_t      szImg[512];       // img= path
    unsigned int nColorkey;        // colorkey hex value
    std::string  szInitCode;       // init_N lines joined with \n
    std::string  szFrameCode;      // code_N lines joined with \n
  };
  std::vector<SpriteEntry> m_spriteEntries;
  int           m_nSpriteSelected = -1;
  void*         m_hSpriteImageList = NULL; // HIMAGELIST (commctrl.h not included here)
  HWND          m_hSpriteList = NULL;
  void          LoadSpritesFromINI();
  void          SaveSpritesToINI();
  void          PopulateSpriteListView();
  void          UpdateSpriteProperties(int sel);
  void          SaveCurrentSpriteProperties();
  HBITMAP       LoadThumbnailWIC(const wchar_t* szPath, int cx, int cy);

  // Pending sprite launches (queued from message handlers, flushed during render when command list is open)
  struct PendingSprite { int nSpriteNum; int nSlot; };
  std::vector<PendingSprite> m_pendingSpriteLoads;

  // Settings window theme
  enum ThemeMode { THEME_DARK = 0, THEME_LIGHT = 1, THEME_SYSTEM = 2 };
  ThemeMode   m_nThemeMode = THEME_DARK;
  bool        IsDarkTheme() const;  // resolves THEME_SYSTEM → actual dark/light
  COLORREF    m_colSettingsBg       = RGB(30, 30, 30);       // Main window background (matches MilkVision)
  COLORREF    m_colSettingsCtrlBg   = RGB(45, 45, 45);       // Edit/combo/list background
  COLORREF    m_colSettingsText     = RGB(0, 220, 0);        // Text color (green, matches MilkVision)
  COLORREF    m_colSettingsDisabled = RGB(128, 128, 128);    // Disabled text
  COLORREF    m_colSettingsBorder   = RGB(60, 60, 60);       // Border/button face
  COLORREF    m_colSettingsBtnFace     = RGB(60, 60, 60);   // Button face
  COLORREF    m_colSettingsBtnHi       = RGB(90, 90, 90);   // 3D highlight edge (top-left)
  COLORREF    m_colSettingsBtnShadow   = RGB(35, 35, 35);   // 3D shadow edge (bottom-right)
  COLORREF    m_colSettingsHighlightText = RGB(255, 255, 255); // Selected tab text
  HBRUSH      m_hBrSettingsBg      = NULL;
  HBRUSH      m_hBrSettingsCtrlBg  = NULL;
  void        LoadSettingsThemeFromINI();
  void        CleanupSettingsThemeBrushes();

  // User "safe" defaults (persisted to INI [UserDefaults] section)
  bool  m_bUserDefaultsSaved = false;
  float m_udOpacity = 1.0f;
  float m_udRenderQuality = 1.0f;
  float m_udTimeFactor = 1.0f;
  float m_udFrameFactor = 1.0f;
  float m_udFpsFactor = 1.0f;
  float m_udVisIntensity = 1.0f;
  float m_udVisShift = 0.0f;
  float m_udVisVersion = 1.0f;
  float m_udHue = 0.0f;
  float m_udSaturation = 0.0f;
  float m_udBrightness = 0.0f;
  float m_udGamma = 2.0f;
  void  SaveUserDefaults();
  void  LoadUserDefaults();
  void  SaveFallbackPaths();
  void  LoadFallbackPaths();

  // Fallback search paths (Files tab)
  std::vector<std::wstring> m_fallbackPaths;
  wchar_t m_szRandomTexDir[MAX_PATH] = {};    // Dedicated random textures directory
  wchar_t m_szContentBasePath[MAX_PATH] = {};  // Base path for textures, sprites, etc.
  // (ResetToFactory, ResetToUserDefaults, UpdateVisualUI, UpdateColorsUI,
  //  RefreshIPCList, NavigatePresetDirUp/Into moved to SettingsWindow)

  // Message autoplay (Messages tab)
  bool    m_bMsgAutoplay = false;
  bool    m_bMsgSequential = false;           // true=sequential, false=random
  float   m_fMsgAutoplayInterval = 30.0f;     // base seconds between messages
  float   m_fMsgAutoplayJitter = 5.0f;        // +/- randomness (seconds)
  bool    m_bMessageAutoSize = true;          // global: auto-fit messages to screen width
  float   m_fNextAutoMsgTime = -1.0f;         // scheduled time for next auto message
  int     m_nNextSequentialMsg = 0;           // index into playback order
  int     m_nMsgAutoplayOrder[MAX_CUSTOM_MESSAGES]; // playback order array
  int     m_nMsgAutoplayCount = 0;            // active messages in order

  // Message overrides (Overrides modal)
  bool    m_bMsgOverrideRandomFont = false;
  bool    m_bMsgOverrideRandomColor = false;
  bool    m_bMsgOverrideRandomSize = false;
  bool    m_bMsgOverrideRandomEffects = false;  // randomize bold/italic
  float   m_fMsgOverrideSizeMin = 10.0f;        // min random size (floor: 0.01)
  float   m_fMsgOverrideSizeMax = 40.0f;        // max random size (ceiling: 100)
  int     m_nMsgMaxOnScreen = 1;                // max concurrent messages (1..NUM_SUPERTEXTS)
  // Animation overrides
  bool    m_bMsgOverrideRandomPos = false;
  bool    m_bMsgOverrideRandomGrowth = false;
  bool    m_bMsgOverrideSlideIn = false;
  bool    m_bMsgOverrideRandomDuration = false;
  bool    m_bMsgOverrideShadow = false;
  bool    m_bMsgOverrideBox = false;
  // Color shifting overrides
  bool    m_bMsgOverrideApplyHueShift = false;
  bool    m_bMsgOverrideRandomHue = false;
  bool    m_bMsgIgnorePerMsgRandom = false;

  // Resource Viewer
  HWND        m_hResourceWnd = NULL;
  HWND        m_hResourceList = NULL;
  static LRESULT CALLBACK ResourceViewerWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
  void        OpenResourceViewer();
  void        PopulateResourceViewer();
  void        LayoutResourceViewer();

  //void  ResetWindowSizeOnDisk();
  bool		LaunchSprite(int nSpriteNum, int nSlot);
  void		KillSprite(int iSlot);
  int         GetNextFreeSupertextIndex();
  void        DoCustomSoundAnalysis();
  void        DrawMotionVectors();

  bool        LoadShaders(PShaderSet* sh, CState* pState, bool bTick, bool bCompileOnly);
  void        UvToMathSpace(float u, float v, float* rad, float* ang);
  void        ApplyShaderParams(CShaderParams* p, LPD3DXCONSTANTTABLE pCT, CState* pState);
  void        RestoreShaderParams();
  void        BuildBindingSlots(CShaderParams* params, const DX12Texture& vsTex, UINT outSlots[32], const DX12Texture* feedbackTex = nullptr, const DX12Texture* imageFeedbackTex = nullptr, const DX12Texture* bufferBTex = nullptr);
  bool        AddNoiseTex(const wchar_t* szTexName, int size, int zoom_factor);
  bool        AddNoiseVol(const wchar_t* szTexName, int size, int zoom_factor);
  bool        AddNoiseTex_ST(const wchar_t* szTexName, int size);
  bool        AddNoiseVol_ST(const wchar_t* szTexName, int size);


  //====[ 3. virtual functions: ]===========================================================================

  virtual void OverrideDefaults();
  virtual void MyPreInitialize();
  virtual void MyReadConfig();
  virtual void MyWriteConfig();
  void SaveWindowSizeAndPosition(HWND hwnd);
  virtual int  AllocateMyNonDx9Stuff();
  virtual void  CleanUpMyNonDx9Stuff();
  virtual int  AllocateMyDX9Stuff();
  virtual void  CleanUpMyDX9Stuff(int final_cleanup);
  virtual void MyRenderFn(int redraw);
  virtual void MyRenderUI(int* upper_left_corner_y, int* upper_right_corner_y, int* lower_left_corner_y, int* lower_right_corner_y, int xL, int xR);
  void ToggleAlwaysOnTop(HWND hwnd);
  void SetOpacity(HWND hwnd);
  bool IsBorderlessFullscreen(HWND hWnd);
  virtual LRESULT MyWindowProc(HWND hWnd, unsigned uMsg, WPARAM wParam, LPARAM lParam);
  void KillAllSprites();
  void KillAllSupertexts();
  bool ChangePresetDir(wchar_t* newDir, wchar_t* oldDir);
  int ToggleSpout();
  int SetSpoutFixedSize(bool toggleSwitch, bool showNotifications);
  virtual void OnAltK();
};

} // namespace mdrop

#endif