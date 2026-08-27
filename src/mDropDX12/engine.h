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
#include <mutex>
#include "texmgr.h"
#include "state.h"
#include "dx12helpers.h"  // DX12Texture
#include "canvas_metric.h"  // CanvasMetric, CanvasSample
#include "video_capture.h" // VideoCaptureSource (needed for unique_ptr complete type)
#include "midi_input.h"   // MidiInput, MidiRow (needed for MIDI members)
#include "tool_window.h"  // DisplaysWindow (needed for unique_ptr complete type)
#include "video_effect_params.h"  // VideoEffectParams, AudioLink
#include "vfx_profile_store.h"    // VFXProfileStore, MAX_VFX_PROFILE_NAME
#include <vector>
#include <array>
#include <map>
#include <string>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <functional>
#include <set>
#include "audio_profile_store.h"
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

typedef enum { TEX_DISK, TEX_VS, TEX_FEEDBACK, TEX_IMAGE_FEEDBACK, TEX_AUDIO, TEX_BUFFER_B, TEX_BUFFER_C, TEX_BUFFER_D, TEX_BLUR0, TEX_BLUR1, TEX_BLUR2, TEX_BLUR3, TEX_BLUR4, TEX_BLUR5, TEX_BLUR6, TEX_BLUR_LAST } tex_code;
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
  float   fShaderSpecLeft[MY_FFT_SAMPLES];   // clean FFT for shader texture (no equalization)
  float   fShaderSpecRight[MY_FFT_SAMPLES];
  static const int RECENT_BUF_MAX = 4096;
  float   recent_buf[3][RECENT_BUF_MAX];
  int     recent_pos[3];
  int     recent_len[3];
  float	  smooth[3];
  float	  smooth_rel[3];
};

// Per-simulation state for independent mirrors (internal fragment — needs
// td_mysounddata/td_vertinfo/MYVERTEX above; stays inside namespace mdrop).
#include "render_context.h"

typedef struct {
  int 	bActive;
  int 	bFilterBadChars;	// if true, it will filter out any characters that don't belong in a filename, plus the & symbol (because it doesn't display properly with DrawText)
  int 	bDisplayAsCode;		// if true, semicolons will be followed by a newline, for display
  int		nMaxLen;			// can't be more than 511
  int		nCursorPos;
  int		nSelAnchorPos;		// -1 if no selection made
  int 	bOvertypeMode;
  wchar_t	szText[48000];      // wide string editing (filenames, user text)
  char	szCode[96000];      // narrow code editing (shader/equation ASCII code)
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
  bool	bExplicitSize = false;	// true if size was explicitly set (skip autosize)
  float fGrowth;			// applies to custom messages only
  int		nFontSizeUsed;		// height IN PIXELS
  int		nTextWidthUsed = 0;	// width IN PIXELS of the rendered text in the title texture
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
  // Scalars, from the active audio profile. See embedded_shaders.h: these
  // are uniforms rather than macros so a profile switch does not stale the
  // shader cache.
  D3DXHANDLE fft_params = NULL;
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
  PShaderInfo bufferC;  // Shadertoy Buffer C (third compute buffer)
  PShaderInfo bufferD;  // Shadertoy Buffer D (fourth compute buffer)
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

// Preset annotation flags (bitmask)
#define PFLAG_FAVORITE  0x01
#define PFLAG_ERROR     0x02
#define PFLAG_SKIP      0x04
#define PFLAG_BROKEN    0x08
// The preset misbehaves as the render canvas grows -- its own feedback maths
// destabilise above the size it was authored for. Recorded as a flag rather
// than left to a HUD notification, because a notification is transient and
// this is a property of the preset worth seeing in a list.
#define PFLAG_CANVAS    0x10

// What produced a stored errorText.  The distinction exists because the two
// kinds age differently: a Shader error is retracted the moment the preset
// compiles clean again, while a Runtime error records a frame that crashed
// while drawing, which a clean compile does not disprove.
enum class PresetErrorKind { Shader, Runtime };

// One rating, tied to the content version it was given against.
//
// Ratings are a list rather than a number so that editing a preset does not
// silently replace the opinion earned by the version before it: a preset rated
// 5, then changed, then rated 2 keeps both, with dates, and reports the average.
struct RatingObservation {
    std::wstring hash;      // content hash this rating was given against
    int          value = 0; // 0-5
    std::wstring when;      // ISO 8601 local time, may be empty on migrated data
};

// Aspect-preserving long-edge cap, 16-aligned. Defined in engine_displays.cpp;
// shared by the mirror swap-chain cap and the feedback-canvas limit.
void CapDimToLongEdge(int& w, int& h, int maxDim);

struct PresetAnnotation {
    std::wstring filename;      // fallback key — filename without path
    std::wstring hash;          // primary key — content identity (preset_hash.h)
    std::vector<std::wstring> paths;  // every location this preset was seen at
    std::vector<RatingObservation> ratings;  // one per content version
    std::wstring lastUsed;      // ISO 8601 local time of the last counted play
    int          useCount = 0;
    int          secondsShown = 0;
    int          rating = 0;    // 0-5, 0 = unrated
    uint32_t     flags = 0;     // PFLAG_ bitmask
    std::wstring notes;
    std::wstring errorText;     // auto-captured from shader compile
    std::wstring errorTime;     // ISO 8601 local time errorText was captured
    PresetErrorKind errorKind = PresetErrorKind::Shader;  // who wrote errorText
    std::vector<std::wstring> tags;  // user-defined tags (e.g., "ambient", "dark")

    // Per-preset override of whatever this preset's tags would have selected,
    // so a special case does not need a tag invented for it.  Resolved
    // independently for each slot: a preset can take its shader from a generic
    // tag rule while naming its own VFX profile.
    //
    // ABSENT AND EMPTY ARE DIFFERENT STATES.  has* false means "inherit from
    // the tags"; has* true with an empty string means "explicitly none", which
    // is how a preset carrying a tag opts out of that tag's rule.  The writer
    // must emit a member only when its flag is set, or every entry in the file
    // silently becomes "none".
    std::wstring shaderOverride;
    std::wstring vfxProfile;

    // Which AudioProfile feeds this preset. Same three states as the two
    // above: absent means inherit from the tags, present-and-empty means
    // explicitly the default profile, which suppresses whatever a tag rule
    // would have selected.
    std::wstring audioProfile;

    // Long-edge cap for the feedback canvas on this preset, in px. Same
    // tri-state as the slots above: absent means inherit (no per-preset
    // limit). It can only REDUCE below the global ceiling -- never raise.
    int  canvasMax = 0;

    // Strength of the feedback damp mitigation, 0..1, 0 = off.
    //
    // The OTHER answer to a preset that destabilises as the canvas grows, and
    // the one to reach for first: rather than shrinking the canvas and losing
    // sharpness, bleed a little energy out of the feedback loop each frame so
    // the preset's own accumulator cannot run away. It is the absolute decay
    // that Flexi's author gave ret.y (`- 0.008`) and did not give ret.z.
    //
    // The applied multiplier is DERIVED FROM THE CANVAS, never fitted: it is
    // exactly 1.0 at the size the preset was authored for and only bites as the
    // canvas grows past it -- see EffectiveFeedbackDamp. A fixed constant would
    // corrupt a render that is already correct at 1080p.
    float feedbackDamp = 0.0f;

    bool hasShaderOverride = false;
    bool hasVfxProfile = false;
    bool hasAudioProfile = false;
    bool hasCanvasMax = false;
    bool hasFeedbackDamp = false;
};

class Engine : public EngineShell {
public:
  MDropDX12* mdropdx12;

  // Messages/Sprites mode helpers
  bool MessagesEnabled() const { return (m_nSpriteMessagesMode & 1) != 0; }
  bool SpritesEnabled()  const { return (m_nSpriteMessagesMode & 2) != 0; }
  // The only writer of m_nSpriteMessagesMode: persists immediately and tells
  // the open tool windows to resync. Toggling messages off from a hotkey used
  // to leave "Show Messages" ticked and the Settings combo stale, so the app
  // looked like it had simply stopped drawing messages.
  void SetSpriteMessagesMode(int mode);

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
  // Opposite-orient milk2/classic: skippable lagged pass. Own list + fence.
  ComPtr<ID3D12Fence>                m_lagIndepFence;
  UINT64                             m_lagIndepSubmitted = 0;
  UINT64                             m_lagIndepSignal = 0;
  UINT                               m_lagIndepAuxFrame = UINT_MAX;
  ComPtr<ID3D12CommandAllocator>     m_lagIndepAlloc;
  ComPtr<ID3D12GraphicsCommandList>  m_lagIndepList;
  bool LagIndepFenceIdle() const;
  bool EnsureLagIndepObjects();
  void ReleaseLagIndepObjects();

  // Dedicated mirror thread + queue (same idea as a second process: own present
  // loop, shared audio/snapshot, not frame-locked to the primary).
  ComPtr<ID3D12CommandQueue>         m_mirrorQueue;
  ComPtr<ID3D12Fence>                m_snapReadyFence;
  UINT64                             m_snapReadyValue = 0;
  ComPtr<ID3D12Fence>                m_mirrorWorkFence;
  UINT64                             m_mirrorWorkSubmitted = 0;
  ComPtr<ID3D12CommandAllocator>     m_mirrorWorkAlloc;
  ComPtr<ID3D12GraphicsCommandList>  m_mirrorWorkList;
  ComPtr<ID3D12Resource>             m_mirrorWorkUpload;
  BYTE*                              m_mirrorWorkUploadPtr = nullptr;
  HANDLE                             m_hMirrorThread = nullptr;
  unsigned                           m_nMirrorThreadId = 0;
  HANDLE                             m_hMirrorWake = nullptr;
  std::atomic<bool>                  m_bMirrorThreadQuit{false};
  std::mutex                         m_mirrorEngineMutex;
  // std::mutex is not FIFO. The render thread re-acquires immediately after each
  // frame, so a try_to_lock worker never got in (measured: 1 orient frame per
  // ~40 s — mirrors looked frozen). The worker raises this before blocking on the
  // lock and the render thread yields to it.
  std::atomic<bool>                  m_bMirrorWorkerWantsLock{false};
  HANDLE                             m_hMirrorFenceEvt = nullptr; // brief wait on in-flight orient GPU work
  // Independent-mirror worker rate cap: 0 = parity (free-run), else max fps.
  // Set from the Displays dropdown / SET_MIRROR_MAXFPS; read by the worker.
  std::atomic<int>                   m_nMirrorMaxFps{0};
  LONGLONG                           m_llLastOrientQpc = 0;   // worker-thread only
  // Audio analysis snapshot for mirror sims: published by the render thread
  // once per frame (after analysis), copied by sim threads under the mutex.
  AudioSnapshot                      m_audioSnap;
  std::mutex                         m_audioSnapMutex;
  void PublishAudioSnapshot();
  void CopyAudioSnapshot(AudioSnapshot& dst);

  // ── Independent mirror simulation (spec: 2026-08-21-independent-mirror-sims) ──
  // Preset bundle: published on the render thread at LoadPresetTick after a
  // successful classic (.milk/.milk2) apply. Mirror sim threads compare
  // versions at frame start and re-import into their own CStates. For .milk2
  // the SPLIT PRESET BODIES travel in memory (Shane, 2026-08-21) — adoption
  // writes them to context-private temp files only for the duration of
  // Import() (its GetFast parser reads a FILE*), then deletes them.
  struct MirrorPresetBundle {
    std::wstring path;          // original preset file (identity/logging)
    bool isMilk2 = false;
    std::string milk2Body1;     // split preset 1 (blend-from), empty if !isMilk2
    std::string milk2Body2;     // split preset 2 (blend-to)
    float milk2Progress = 0.5f; // frozen blend progress
    bool  milk2HasRandoms = false;
    float milk2Random[5] = {};  // MD3 locks shader rand_preset to these
    float blendTime = 0.f;      // primary's transition duration (transient blends)
    uint32_t version = 0;
  };
  MirrorPresetBundle                 m_presetBundle;
  std::mutex                         m_presetBundleMutex;
  std::atomic<uint32_t>              m_presetBundleVersion{0};
  // Serializes every CState::Import across threads: the GetFast line cache
  // keys on consecutively-allocated FILE*s (see engine_presets.cpp:2267), so
  // two concurrent Imports would poison each other. Held by the async preset
  // loaders and by mirror adoption. NEVER acquire m_mirrorEngineMutex while
  // holding this (LoadPresetTick joins a loader while holding the engine
  // mutex — nesting the other way would deadlock).
  std::mutex                         m_stateImportMutex;
  MirrorSimContext                   m_mirrorSim; // stage 1: one context
  std::string                        m_pendingMilk2Body1, m_pendingMilk2Body2;
  void PublishPresetBundle(bool isMilk2, float blendTime);
  // Adoption phase 1 (no engine mutex): import bundle into ctx states.
  // Returns true if a new preset was adopted (caller must regenerate the
  // ctx blend mesh under the engine mutex — see m_bMirrorSimPatternDirty).
  bool MirrorSimAdoptPreset(MirrorSimContext& c);
  void MirrorSimStepFrame(MirrorSimContext& c);       // per-frame EEL + mesh, ctx-pure
  void MirrorSimEnsureGrid(MirrorSimContext& c, int w, int h,
                           int aspectW = 0, int aspectH = 0);
  void MirrorSimApplyBlendPattern(MirrorSimContext& c); // engine mutex held (record path)
  void MirrorSimFree(MirrorSimContext& c);
  void ComputeGridAlphaValuesCtx(MirrorSimContext& c);
  void LoadPerFrameEvallibVarsCtx(MirrorSimContext& c, CState* pState);
  // Mirror throughput (observability: DIAG_MIRRORS + one log line per second).
  // Opposite-orient runs on its own thread and is deliberately not frame-locked,
  // so its rate is expected to differ from the primary and from each panel.
  std::atomic<unsigned>              m_nOrientFrameAccum{0};
  std::atomic<unsigned>              m_nMirrorPresentAccum{0};
  float                              m_fOrientFps = 0.0f;
  float                              m_fMirrorPresentFps = 0.0f;
  DWORD                              m_dwMirrorFpsTick = 0;
  // Jerkiness hunt (2026-08-23): rate alone says nothing about smoothness —
  // a steady 22 fps looks fine, 22 fps with a 60 ms outlier does not. Every
  // value is microseconds over the last 1 s window, published by the worker
  // (dt/lock/rec/sim) and by the render thread (primary frame interval).
  std::atomic<int> m_diagOrientDtMinUs{0};   // worker iteration interval
  std::atomic<int> m_diagOrientDtMaxUs{0};
  std::atomic<int> m_diagOrientDtAvgUs{0};
  std::atomic<int> m_diagOrientLockMaxUs{0}; // spin-wait for m_mirrorEngineMutex
  std::atomic<int> m_diagOrientLockAvgUs{0};
  std::atomic<int> m_diagOrientRecMaxUs{0};  // record under the engine lock
  std::atomic<int> m_diagOrientRecAvgUs{0};
  std::atomic<int> m_diagOrientSimMaxUs{0};  // MirrorSimStepFrame (lock-free)
  std::atomic<int> m_diagOrientSimAvgUs{0};
  std::atomic<int> m_diagOrientFenceWaits{0};// fence-gate misses (wasted wakes)
  std::atomic<int> m_diagPrimDtMinUs{0};     // primary frame interval
  std::atomic<int> m_diagPrimDtMaxUs{0};
  std::atomic<int> m_diagPrimDtAvgUs{0};
  void LockMirrorEngine() override;
  void UnlockMirrorEngine() override;
  void StartMirrorThread();
  void StopMirrorThread();
  void SignalMirrorSnapAndWake();
  void MirrorThreadMain();
  void MirrorThreadDrawAndPresent();
  // Panel output stays entirely on the RENDER thread (worker-side blits and
  // presents were tried 2026-08-21 and produced a driver TDR on the mirror
  // queue, then a D3D12Core queue-mutex deadlock on the main queue — never
  // submit or Present from the worker on the main queue). Instead the worker
  // double-buffers its display target and PUBLISHES the completed index once
  // its fence proves the frame done; the render thread always blits the
  // published image — the stretched-primary fallback (the two-image flicker)
  // is gone. -1 = nothing published yet (warmup: stretch fallback allowed).
  std::atomic<int>                   m_orientPublishedIdx{-1};
  // Freshness token for the render thread's "did the worker publish a new
  // frame?" test. It MUST NOT be the disp[] slot: that index rotates mod 3, so
  // whenever the worker publishes at ~3x (or 6x) the render thread's sampling
  // rate, every sample lands on the same slot, the draw is skipped, and the
  // panels hold one image for a second at a time — the jerky mirrors of
  // 2026-08-23 (sim 240 fps, panels 0-2 fps). This counter only ever grows, so
  // equality means "nothing new" and nothing else. Never reset it: after an
  // orient-pipe recreate a stale panel value merely forces a redraw.
  std::atomic<unsigned>              m_orientPublishedSeq{0};
  // Primary frame counter, bumped once per render-thread frame. In parity mode
  // (m_nMirrorMaxFps == 0) the worker steps its sim exactly once per advance of
  // this, so the mirrors integrate their feedback at the SAME rate as the
  // window they mirror. Free-running the worker made them run visibly faster
  // (2026-08-23: 160 fps worker against a 40 fps capped primary).
  std::atomic<unsigned>              m_nPrimaryFrameSeq{0};
  unsigned                           m_nWorkerSeenPrimaryFrame = 0; // worker-only
  UINT m_diagOrientAspectBad = 0;   // flicker hunt: ctx shape draws w/ wrong aspect
  UINT m_diagOrientAspectGood = 0;
  int                                m_orientLastWrite = -1;   // worker-only
  std::atomic<bool> m_bOrientImageReady{false};
  std::atomic<int>  m_orientNeedW{0};
  std::atomic<int>  m_orientNeedH{0};
  // Leader panel's RAW dims (pre 1920-cap, pre 16-align). The sim grid takes
  // its aspect from these: the rounded pipe (1920x1088 = 1.7647) vs the panel
  // (2560x1440 = 1.7778) reads as slightly-flat circles otherwise.
  std::atomic<int>  m_orientPanelW{0};
  std::atomic<int>  m_orientPanelH{0};
  bool m_bMirrorClassRegistered = false;
  bool m_bMirrorsActive = false;       // Displays tab button; always starts off
  // Render-thread countdown after EnsureFullScreen. Create swap chains only
  // once the primary SC is stable — same-turn Init+ResizeSwapChain TDRs.
  std::atomic<int> m_nDeferMirrorActivate{0};
  // One-shot: after mirror on/off, bring primary + mirrors to front (respects AOT for sticky topmost).
  std::atomic<bool> m_bRaiseMirrorsNextFrame{false};
  void RaiseMirrorSurfaces(HWND hPrimary);
  bool m_bMirrorWatermarkActive = false; // True while in mirror watermark mode (App.cpp manages)
  wchar_t m_szWatermarkRenderDevice[32] = {}; // Device name of the display render moved to (for deterministic skip)
  bool m_bWatermarkActive = false;       // True while in single-window watermark mode (App.cpp manages)
  bool m_bMirrorModeForAltS = false;   // When true, ALT-S activates mirrors+fullscreen instead of stretch
  bool m_bMirrorPromptDisabled = false; // Skip "no mirrors enabled" prompt; auto-enable all
  bool m_bMirrorPromptActive = false;   // Guard: prompt already showing
  // Global default for new/all monitors: independent re-render (correct aspect).
  // Per-output config.bIndependentRender is the effective flag after load.
  bool m_bMirrorIndependentDefault = false;
  std::atomic<bool> m_bMirrorStylesDirty{false}; // UI thread sets; render thread applies
  // UI/hotkey request: tear down & recreate monitor mirrors on render thread only
  // (never WaitForGpu / DestroyDisplayOutput from the UI thread — that freezes the PC)
  std::atomic<bool> m_bMirrorForceReinit{false};
  // Soft-reset orient frame counters on next render pass (never free RTs on toggle).
  std::atomic<bool> m_bMirrorResetOrientNextFrame{false};
  std::atomic<bool> m_bMirrorIndepSizeDirty{false}; // resize SCs after independent toggle
  std::atomic<int> m_nDeferMirrorResize{0}; // skip ResizeBuffers after primary FS/idle
  // Reused 32-slot SRV block for classic letterbox (avoids heap growth / freeze)
  UINT m_mirrorLetterboxSrvBase = UINT_MAX;
  UINT m_mirrorLetterboxSrvEpoch = 0; // invalidate letterbox SRV after descriptor rewind

  // Last SendToDisplayOutputs frame summary (DIAG_MIRRORS only)
  int  m_mirrorDiagMainW = 0, m_mirrorDiagMainH = 0;
  int  m_mirrorDiagMainPortrait = 0;
  int  m_mirrorDiagNeedMainSrv = 0;
  int  m_mirrorDiagCanSampleMain = 0;
  int  m_mirrorDiagAnyOpposite = 0;
  int  m_mirrorDiagAnyIndepMilk3 = 0;
  int  m_mirrorDiagShadertoy = 0;
  int  m_mirrorDiagCompPso = 0;
  unsigned m_mirrorDiagAllocHr = 0;
  unsigned m_mirrorDiagListHr = 0;
  unsigned m_mirrorDiagSkipFrames = 0;
  unsigned m_mirrorDiagFrameCounter = 0;
  unsigned m_mirrorDiagAuxUsed = 0;
  int  m_mirrorDiagSlotCount = 0;

  enum MirrorActivateResult { MirrorActivated, MirrorFullscreenOnly, MirrorCancelled };
  MirrorActivateResult TryActivateMirrors(HWND hRenderWnd);

  void EnumerateDisplayOutputs();
  void LoadDisplayOutputSettings();
  void SaveDisplayOutputSettings();
  void InitDisplayOutput(DisplayOutput& out);
  void DestroyDisplayOutput(DisplayOutput& out);
  // Full GPU/window teardown of one mirror (frees its reserved RTV block).
  // Every path that drops a MonitorMirrorState must go through this — a plain
  // unique_ptr reset leaks the RTV reserve and the borderless popup HWND.
  void DestroyMonitorMirror(MonitorMirrorState& ms);
  // Mirrors detached by EnumerateDisplayOutputs (UI thread) wait here until the
  // render thread can destroy them (WaitForGpu/DestroyWindow are render-owned).
  std::mutex m_orphanMirrorMutex;
  std::vector<std::unique_ptr<MonitorMirrorState>> m_orphanMirrors;
  void DrainOrphanedMirrors();
  void DestroyAllDisplayOutputs();
  void ReleaseDisplayOutputWraps();
  void ResizeMirrorSwapChain(MonitorMirrorState& ms, int newW, int newH);
  // Draw + Present mirrors. Same-orient / copy-mode: cheap blit or CopyResource.
  // Opposite-orient independent: lagged fenced warp+comp (skip if still in flight).
  void SendToDisplayOutputs() override;
  // Milk3 image pass to mirror RT (native size). Shares same audio uniforms.
  // Fallback when orient pipeline is unavailable; uses primary feedback (shared history).
  void RenderMilk3ImageToMirror(ID3D12GraphicsCommandList* cmdList,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv, int monW, int monH);
  // Second orientation milk3 pipeline (portrait-sized feedback when primary is landscape).
  // Owns Buffer A–D + Image feedback at native opposite-orient size; full A→…→Image each frame.
  struct Milk3OrientPipeline {
    DX12Texture fbA[2], fbB[2], fbC[2], fbD[2], imgFb[2];
    // Dedicated display rotation, both modes: classic comp renders straight
    // into disp[dispWrite]; milk3 copies its final Image into it. THREE faces
    // so the published one is never rewritten while a late-recorded render-
    // thread blit may still sample it (two faces raced: write N returns at
    // N+2 while blits of the N-publish could still be in flight — the
    // "brief flashes" of 2026-08-21).
    DX12Texture disp[3];
    int  dispWrite = 0;
    // Classic independent: blur pyramid at orient size (GetBlur1/2/3). Without this,
    // presets like blue haze sample primary landscape blur → soft/wrong haze.
    DX12Texture blur[6];
    int  blurW[6] = {};
    int  blurH[6] = {};
    int  w = 0, h = 0;
    int  fbIdx = 0;
    int  frames = 0;
    UINT bindBase = UINT_MAX; // 5×32 SRV slots (A,B,C,D,comp) + 1 blit block
    UINT bindEpoch = 0;
    bool ready = false;
  };
  Milk3OrientPipeline m_orientPipe;
  bool EnsureOrientPipeline(int w, int h);
  void ReleaseOrientPipeline();
  // Full milk3 Buffer A–D + Image into internal imgFb only (never a swap-chain face).
  // outW/outH select aspect / size; result is read via BlitOrientOutputToMirror.
  bool RenderMilk3OrientPipeline(ID3D12GraphicsCommandList* cmdList, int outW, int outH);
  // Classic .milk independent: VS ping-pong in imgFb, comp to fbA[0] for display.
  // Same user-facing independent mode as milk3.
  bool RenderClassicOrientPipeline(ID3D12GraphicsCommandList* cmdList, int outW, int outH);
  // When set, textured shapes sample this instead of m_dx12VS[0] (orient re-render).
  const DX12Texture* m_pShapeVsOverride = nullptr;
  // Classic orient: true when re-rendering opposite aspect (more work already in aux).
  // Same-orient re-render can spend more of the aux buffer on shapes.
  bool m_bOrientOppositeAspect = false;
  // Stretch-blit latest orient imgFb → mirror RT (full clear + full viewport).
  // Safe source: never samples a flip-model back buffer (that caused leader-only
  // ghost bands after running a while when peers blitted from leader.bb).
  bool BlitOrientOutputToMirror(ID3D12GraphicsCommandList* cmdList,
                                D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH);
  // Lock m_nTexSize + aspect to primary VS (clears GetWidth override). Call at the
  // start of every primary frame so mirror SizeGuard cannot leak portrait dims.
  void RestorePrimaryTexSizeFromVS() override;
  // Intermediate copy of primary BB for stretch/letterbox — never SRV the flip
  // surface (sampling flip BB between Execute and Present ghosts mirrors).
  DX12Texture m_mirrorSrcTex;
  bool m_bMirrorSrcCopiedThisFrame = false;
  // Copy flip BB → m_mirrorSrcTex on the *primary* list while still RT,
  // before Execute. SendToDisplayOutputs must not touch the flip BB.
  void CopyPrimaryToMirrorSrc();
  bool ShouldHideMirrorHud() const override { return m_bDisableMirrorHud && m_bMirrorsActive; }
  bool AnyIndependentMirrorEnabled() const override;
  void DrawDeferredMessages() override;
  // Supertext messages + HUD (+ optional sprites) onto a mirror RT.
  void DrawOverlaysToMirror(ID3D12GraphicsCommandList* cmdList, int monW, int monH,
                            bool drawSprites = true);
  // Clear every mirror SC back buffer (PRESENT/COMMON → clear → restore). Stops
  // flip-chain ghosts when switching landscape stretch → portrait orient.
  void ClearMirrorSwapChainAllBuffers(ID3D12GraphicsCommandList* cmdList,
                                      MonitorMirrorState& ms, UINT keepAsRtvIndex);

  // Blit main BB → mirror RT. mainBB must be PIXEL_SHADER_RESOURCE.
  // scaleMode: 0 = stretch-fill, 1 = letterbox/fit (bars), 2 = cover/crop (fill, no bars).
  void BlitMainToMirror(ID3D12GraphicsCommandList* cmdList,
                        ID3D12Resource* mainBB, int mainW, int mainH,
                        D3D12_CPU_DESCRIPTOR_HANDLE mirrorRtv, int monW, int monH,
                        int scaleMode = 0);
  void ToggleMirrorIndependentRender();
  void SetMirrorIndependentRender(bool enable); // absolute set (IPC SET_MIRROR_INDEPENDENT=)
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

  // Dynamic F1 help text (generated from binding table, all pages in one buffer)
  wchar_t m_szHelpAll[16384] = {};
  int     m_nHelpLineCount = 0;   // total lines in m_szHelpAll
  void GenerateHelpText();

  // Help display category order (user-configurable)
  int  m_helpCatOrder[HKCAT_COUNT] = {};
  void ResetHelpCatOrder();
  void LoadHelpCatOrder();
  void SaveHelpCatOrder();

  // Dynamic user-added hotkeys (Script Commands and Launch Apps)
  std::vector<UserHotkey> m_userHotkeys;
  int m_nextUserHotkeyId = USER_HOTKEY_ID_BASE;
  int  AddUserHotkey(UserHotkeyType type);          // returns index in m_userHotkeys
  void RemoveUserHotkey(int index);
  void LaunchOrFocusApp(const std::wstring& path);

  // Idle timer (screensaver mode)
  bool m_bIdleTimerEnabled = false;
  int  m_nIdleTimeoutMinutes = 5;     // 1-60 minutes
  int  m_nIdleAction = 0;             // 0 = Fullscreen, 1 = Stretch/Mirror, 2 = Mirror all
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

  // Parallel evaluation of the per-vertex (per_pixel) equations. The pool is
  // started lazily on first use and torn down with the engine; the calling
  // render thread participates as worker 0, so a count of 1 means "serial" and
  // no thread is ever created. See pv_workers.h for the measurements that
  // motivated this and the argument that it is safe.
  mdrop::PvThreadPool m_pvPool;
  int  GetPerVertexWorkerCount();   // includes the calling thread; 1 = serial
  void ShutdownPerVertexPool();
  float   m_fHudFontUserScale = 1.0f;  // 0.10..2.00 extra HUD size (Settings)
  bool    m_bDisableMirrorHud = false; // Settings: no HUD/overlays on mirrors
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
  bool    m_bEnablePresetStartup = true;
  bool    m_bEnableAudioCapture = true;
  float   m_fAudioSensitivity = 1.0f;   // 1.0 = passthrough (default), >1 = manual gain boost
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
  int m_bStartFullscreen = 0;
  // Point on the monitor that was fullscreen when the app last exited.
  int m_FullscreenHintX = 0;
  int m_FullscreenHintY = 0;
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
  bool m_bShowNotifications = true;  // false = suppress all HUD notifications

  // Error Display Settings
  float   m_ErrorDuration       = 8.0f;     // seconds

  // FFT EQ Smoothing (Milkwave Remote)
  float   m_fFFTAttackGlobal    = 0.5f;     // attack rate (0..1), set via IPC or INI
  float   m_fFFTDecayGlobal     = 0.5f;     // decay rate (0..1), set via IPC or INI
  bool    m_bFFTSmoothingActive = false;     // true once Remote sends FFT params
  float   m_fFFTSmoothed[MY_FFT_SAMPLES];   // smoothed spectrum per bin
  float   m_fFFTPeak[MY_FFT_SAMPLES];       // peak hold per bin
  int     m_nFFTPeakHold[MY_FFT_SAMPLES];   // frames remaining in peak hold

  int m_MinPSVersionConfig = 4; // MD2_PS_3_0: DX12 requires ps_3_0 minimum (ps_2_a silently drops texture bindings)
  int m_MaxPSVersionConfig = 6;
  bool m_ShowUpArrowInDescriptionIfPSMinVersionForced = false;

  // GPU Protection Settings
  int  m_nMaxShapeInstances = 0;         // Cap per-shape instance count (0=unlimited, e.g. 512)
  bool m_bScaleInstancesByResolution = false; // Scale down num_inst at resolutions above base
  int  m_nInstanceScaleBaseWidth = 1920; // Reference width for instance scaling (instances scale down above this)
  bool m_bSkipHeavyPresets = false;      // Auto-skip presets exceeding GPU safety thresholds
  int  m_nHeavyPresetMaxInstances = 4096; // Total shape instances across all shapes that triggers skip
  // Exit when available local VRAM (budget-based) falls below this percent. 0 = disabled.
  int  m_nMinAvailableVramPercent = 5;
  // Last DXGI local VRAM sample (for debug overlay / exit message)
  UINT64 m_vramBudgetBytes = 0;
  UINT64 m_vramUsageBytes = 0;
  float  m_vramAvailablePercent = 100.0f;
  bool   m_bVramExitTriggered = false;

  //bool		m_bAlways3D;
  //float       m_fStereoSep;
  //bool		m_bAlwaysOnTop;
  //bool		m_bFixSlowText;
  //bool		m_bWarningsDisabled;		// messageboxes
  bool		    m_bWarningsDisabled2;		// warnings/errors in upper-right corner (m_szUserMessage)
  bool        m_bAnisotropicFiltering;
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

  // ── Compiled-shader line mapping (for the Preset Editor's error display) ──
  // LoadShaderFromMemory prepends include.fx and the per-type #defines, then
  // injects a few lines INSIDE the preset's shader_body (the "void PS(...)"
  // line, "float3 ret = 0;", and for comp a 4-line rad/ang block).  D3DCompile
  // reports line numbers in that assembled text, dozens of lines off from what
  // the user typed.  These record the shift so the editor can point at the
  // right line.  Written on the render thread during a shader compile.
  int              m_nShaderPreludeLines = 0;   // lines before the preset's own text
  std::vector<int> m_shaderInjectedLines;       // compiled-text lines the engine inserted
  std::wstring     m_wLastShaderError;          // raw D3DCompile text of the last failure
  // 1-based compiled line -> 1-based user line, or 0 if the line lies in
  // engine-generated prelude rather than anything the user wrote.
  int MapCompiledLineToUserLine(int nCompiledLine) const;

  // ── Preset Editor apply/save (RENDER THREAD ONLY) ──
  // Reached via RenderCmd::ApplyPresetCode / RenderCmd::SavePresetFile.  Never
  // call these from a ToolWindow thread: they touch m_pState, m_shaders and the
  // DX12 PSOs, all of which belong to the render thread.
  // nSide is a PresetSide: PSIDE_LIVE edits m_pState, PSIDE_BLENDFROM edits
  // m_pOldState (preset 1 of a frozen .milk2, which renders every frame too).
  bool ApplyPresetCodeSection(int section, int index, const char* code, int nSide = 0);
  // True when a .milk2 is on screen and both of its presets are being rendered,
  // so preset 1 is a real, editable, visible thing.
  bool HasEditableBlendFromPreset() const {
    return m_bMilk2FrozenBlend && m_pOldState != nullptr;
  }
  bool SavePresetToPath(const wchar_t* szPath);
  // Write both presets of a frozen .milk2 back out in the wrapper format.
  bool SaveMilk2ToPath(const wchar_t* szPath);
  // Replace the live preset from complete .milk text (the editor's whole-file
  // and raw views).  Leaves m_szCurrentPresetFile alone so Save still targets
  // the real file rather than the temp one this writes.
  bool ApplyPresetTextToState(const std::wstring& milkText, int nSide = 0);

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
  // True while the Preset Editor window is open.  AutoPresetChangesAllowed()
  // reads it to suppress auto-advance so an unsaved edit cannot be discarded by
  // a preset change.  Atomic: set from the editor's own thread, read on the
  // render thread every frame.
  std::atomic<bool> m_bPresetEditorOpen{false};

  // Testing mode: freeze everything that changes the frame without being asked
  // -- the timed advance, the audio hard cuts, the preset change on song
  // change, the idle timer -- and ignore the keyboard, so a stray keystroke
  // cannot alter a measurement in progress. Explicit IPC still works; the
  // point is to remove what the harness did not ask for.
  //
  // Deliberately NOT persisted. A testing flag that came back after a restart
  // would be worse than not having one, and it is the rule the VFX profile
  // store already settled on: nothing is written unless explicitly saved.
  bool    m_bTestingMode = false;
  // The frame cap in force before testing mode pinned it, so leaving testing
  // mode gives the user's cap back. -1 = nothing to restore.
  int     m_nFpsCapBeforeTesting = -1;
  // Time of the first ESC. Leaving testing mode takes two presses, because one
  // stray key must not end a run that has been going for minutes.
  float   m_fTestingEscapeArmedTime = -1.0f;
  // Latched on the first entry into testing mode and never cleared. Leaving
  // testing mode gives the keyboard and the auto-advance back, but it cannot
  // give back what the harness did to the settings held in memory -- the render
  // window has been moved and resized, the preset directory repointed. The
  // shutdown auto-save would then write all of that out as if the user had
  // arranged it, which is how settings.ini ended up holding a test rig's
  // 720x540 window at x=-1680. With this latched, that save is skipped; nothing
  // a person did is lost, because every settings control persists the moment it
  // is changed rather than waiting for shutdown.
  bool    m_bTestingModeUsedThisSession = false;
  // Opt out of the write shield, for the rare test that needs its settings
  // changes to land on disk. Set by [Milkwave] TestingModeWritesSettings=1 or
  // by sending TESTING_MODE=1,persist.
  bool    m_bTestingModeWritesSettings = false;
  // Preset loads compile asynchronously, so "freeze auto preset changes" has a
  // hole: an AUTO-initiated load already compiling when the freeze lands (or
  // when the user hits preset lock) still applies seconds later and the frame
  // changes anyway -- observed live during an A/B session. The timer path tags
  // its loads via m_bNextLoadIsAuto; LoadPreset() captures the tag into
  // m_bLoadingInitiatedByAuto; LoadPresetTick() DISCARDS a finished auto load
  // if AutoPresetChangesAllowed() has gone false since it started. Explicit
  // loads (IPC, hotkeys, browser) are never tagged and always apply.
  bool    m_bNextLoadIsAuto = false;
  bool    m_bLoadingInitiatedByAuto = false;
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
  int     m_nDiagDisplayMode = 0;  // 0=normal, 1=show VS[0] raw, 2=show VS[1] raw
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
  // Shader compile errors recorded since this load began.  Zero at apply time
  // is what licenses ClearPresetShaderError to retract a stale error flag.
  std::atomic<int> m_nShaderErrorsThisLoad{0};
  float   m_fLoadStartTime = 0;              // GetTime() when async load began (for timeout)
  float   m_fShaderCompileTimeout = 8.0f;    // seconds before auto-skipping a stuck compilation
  bool    m_bLoadingShadertoyMode = false;    // true when async load is for a .milk3 Shadertoy preset
  bool    m_bLoadingMilk2 = false;            // true when async load is a .milk2 double-preset
  int     m_nPresetsLoadedTotal; //important for texture eviction age-tracking...
  CState	m_state_DO_NOT_USE[4];	// do not use; use pState and pOldState instead.
  CState* m_pMilk2OldState;      // 4th CState for .milk2 preset 1 (old/blend-from state)
  PShaderSet m_Milk2OldShaders;   // preset 1's shaders during .milk2 async load
  int     m_nMilk2MixType = -1;   // blend pattern from .milk2 metadata
  // Which of MD3's three plasma branches to run: 0 plasma, 1 plasma2,
  // 2 plasma3. They share GenPlasma and the corner seeds; see mixtype 2.
  int     m_nMilk2PlasmaVariant = 0;
  char    m_szMilk2Pattern[32] = {};  // raw blending_pattern name from the .milk2 header
  bool    m_bMilk2DeterministicField = false;  // suppress RandomizeBlendPattern's
                                               // process-wide time accumulators
  bool    m_bMilk2FrozenBlend = false;  // true when .milk2 blend is permanently frozen
  float   m_fMilk2FrozenProgress = 0.5f; // frozen blend progress from .milk2 metadata
  float   m_fMilk2Random[5] = {};       // blending random_1..5 from the .milk2 header
  bool    m_bMilk2HasRandoms = false;   // true when the file provided random_1..5
  bool    m_bMilk2UseSavedRandoms = false; // RandomizeBlendPattern should consume saved values
  int     m_nMilk2Direction = 1;        // blending_direction from the .milk2 header
  wchar_t m_szMilk2Temp1[MAX_PATH] = {};  // temp file for preset 1 (deleted after load)
  wchar_t m_szMilk2Temp2[MAX_PATH] = {};  // temp file for preset 2 (deleted after load)
  // MilkDrop 3.28 [SPRITEn] blocks (file-level, last index wins).
  struct Milk2SpriteDesc {
    int          nIndex = 1;
    wchar_t      szName[MAX_PATH] = {};
    unsigned int nColorKey = 0;
    int          nLayer = 0;
    int          nBlend = 0;
    float        fAlpha = 1.0f;
    float        fBurn = 1.0f;
    float        fX = 0.0f;
    float        fY = 0.0f;
    // MD3 defaults SpriteSX/SY to -0.5f (0xbf000000). Not adopted yet:
    // the fitted scale curve in milkdropfs.cpp was calibrated with 1.0f
    // here, so changing both at once confounds them.
    float        fSX = 1.0f;
    float        fSY = 1.0f;
    float        fRot = 0.0f;
    float        fSpeed = 0.0f;
    float        fRepeatX = 1.0f;
    float        fRepeatY = 1.0f;
    std::string  szInit;
    std::string  szCode;
  };
  std::vector<Milk2SpriteDesc> m_milk2Sprites;
  bool    m_bMilk2SpritesNeedApply = false;
  static const int MILK2_SPRITE_USERDATA = 0x4D4B3200; // 'MK2\0'
  void        KillMilk2Sprites();
  void        ApplyMilk2Sprites();
  bool        LaunchMilk2Sprite(const Milk2SpriteDesc& spr);
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
  int			m_nSubdirMode = 0;		// 0=never include subdirs, 1=ask, 2=always include
  bool		m_bRecursivePresets = false;	// true when current list was built recursively
  std::wstring m_szTagFilter;		// if non-empty, only show presets with this tag
  std::wstring m_szActivePresetList;	// if non-empty, currently loaded preset list name
  int			m_nPresetListCurPos;// Index of the currently-HIGHLIGHTED preset (the user must press Enter on it to select it).
  int			m_nCurrentPreset;	// Index of the currently-RUNNING preset.
  //   Note that this is NOT the same as the currently-highlighted preset! (that's m_nPresetListCurPos)
  //   Be careful - this can be -1 if the user changed dir. & a new preset hasn't been loaded yet.
  wchar_t		m_szCurrentPresetFile[512];	// w/o path.  this is always valid (unless no presets were found)
  wchar_t		m_szPendingStartupSave[512] = {};  // preset path waiting to be persisted after 5s render time
  float		m_fPendingStartupSaveTime = 0;     // GetTime() when preset was loaded (0 = no pending save)
  PresetList  m_presets;

  // Pending preset data — scan thread writes, render thread swaps in
  PresetList              m_pendingPresets;
  int                     m_nPendingPresets = 0;
  int                     m_nPendingDirs = 0;
  int                     m_nPendingCurPos = 0;
  bool                    m_bPendingListReady = false;
  std::atomic<bool>       m_bPendingPresetSwap{false};

  // Pending ratings (pass 2)
  std::vector<float>      m_pendingRatings;
  int                     m_nPendingRatingsCount = 0;
  std::atomic<bool>       m_bPendingRatingsSwap{false};

  // Preset annotation system (presets.json)
  std::unordered_map<std::wstring, PresetAnnotation> m_presetAnnotations;
  bool m_bAnnotationsDirty = false;
  void LoadPresetAnnotations();
  void SavePresetAnnotations();
  PresetAnnotation* GetAnnotation(const wchar_t* filename, bool create = false);
  void SetPresetFlag(const wchar_t* filename, uint32_t flag, bool set);
  void SetPresetNote(const wchar_t* filename, const std::wstring& note);
  void SetPresetTags(const wchar_t* filename, const std::vector<std::wstring>& tags);

  // The per-preset override slots. `present` false REMOVES the member, so the
  // preset inherits from its tags again; `present` true with an empty name is
  // "explicitly none". Two different states, deliberately not one argument.
  void SetPresetShaderOverride(const wchar_t* filename, const std::wstring& name,
                               bool present);
  void SetPresetVFXProfile(const wchar_t* filename, const std::wstring& name,
                           bool present);
  void SetPresetAudioProfile(const wchar_t* filename, const std::wstring& name,
                             bool present);
  int  ImportMWRTags(const wchar_t* szTagsJsonPath);  // returns count of presets updated
  void CollectAllTags(std::vector<std::wstring>& allTags) const;  // unique sorted list of all tags

  // Content-hash identity (preset_hash.h).  Hash is the primary key; filename
  // is the fallback, and the two cover each other: the hash survives moving and
  // renaming, the filename survives the file being edited.
  std::unordered_map<std::wstring, std::wstring> m_annotationsByHash;  // hash -> map key
  PresetAnnotation* GetAnnotationByHash(const std::wstring& hash);
  PresetAnnotation* ResolveAnnotation(const wchar_t* filename, const wchar_t* hash,
                                      const wchar_t* fullPath, bool create);
  void RebuildAnnotationHashIndex();
  // Merges "from" into "into" without discarding anything (see the design spec
  // section 3.4); notes/errorText are the only fields where one text wins.
  static void MergeAnnotations(PresetAnnotation& into, const PresetAnnotation& from);

  // ── Keeping test runs out of presets.json ──
  //
  // A harness run loads hundreds of throwaway presets, and every one of them
  // that stayed on screen past the usage threshold used to mint an entry.  The
  // database then describes the test rig rather than the user's library.
  //
  // Two independent gates, because they answer different questions:
  //   * testing mode  -- "is a measurement running right now"
  //   * ignored dirs  -- "is this file a scratch preset regardless of mode"
  // Both only ever block CREATION and writes.  An entry that already exists
  // still resolves and still reads, so a test run never hides real data.
  //
  // Folder names, matched against any single path segment, case-insensitively.
  // Semicolon-separated in the INI so the TEST/ convention is configurable
  // rather than a magic string compiled in.
  std::vector<std::wstring> m_annotIgnoreDirs;
  // The raw INI string, kept so it can be written back unchanged.
  wchar_t m_szAnnotIgnoreDirs[512] = L"TEST";
  void ParseAnnotIgnoreDirs(const wchar_t* semicolonList);
  bool IsAnnotationIgnoredPath(const wchar_t* fullPath) const;
  // Testing mode + an ignored directory = scratch: no identity, so no tags,
  // no rating and no per-preset overrides. Counterpart to
  // ShouldSkipAnnotationWrite, which gates writes.
  bool IsScratchPreset(const wchar_t* fullPath) const;
  // The one predicate both gates funnel through. fullPath may be NULL/empty,
  // in which case only the testing-mode gate can fire.
  bool ShouldSkipAnnotationWrite(const wchar_t* fullPath) const;

  // ── Duplicate detection ──
  //
  // One group per content hash that has more than one file on disk.  Built by
  // an explicit scan, never implicitly: it reads every preset file under a
  // root, which is far too expensive to do on a timer.
  struct DuplicateFile {
    std::wstring path;
    uint64_t     sizeBytes = 0;
    FILETIME     written{};      // last-write time, for "keep the newest"
  };
  struct DuplicateGroup {
    std::wstring hash;
    std::wstring displayName;    // basename of the first file found
    std::vector<DuplicateFile> files;
  };
  // Progress callback; return false to cancel.
  //
  // A scan has two phases and they need telling apart. While the tree is being
  // walked, `total` is 0 and `done` is the number of preset files found so far;
  // once hashing starts, `total` is the final count and `done` counts up to it.
  // Without the distinction the walk could not be cancelled at all -- and on a
  // deep or networked tree the walk is the slow half.
  using DupeScanProgressFn = std::function<bool(int done, int total, const wchar_t* current)>;
  // Returns every group with 2+ files, largest group first. `root` is walked
  // recursively. Groups are found by CONTENT, so a renamed copy still matches.
  //
  // const, and it touches no member: it is meant to run on a worker thread
  // while the UI thread keeps painting. Handing the result back through
  // AdoptDuplicateScan keeps every WRITE to the caches on one thread, so the
  // two never need a lock between them.
  std::vector<DuplicateGroup> ScanForDuplicatePresets(const wchar_t* root,
                                                      const DupeScanProgressFn& onProgress,
                                                      std::set<std::wstring>* outAllHashes) const;
  // Publish a finished scan into the session caches. Call on the thread that
  // reads them (the Annotations window), never from the worker.
  void AdoptDuplicateScan(const std::vector<DuplicateGroup>& groups,
                          std::set<std::wstring>&& allHashes);
  // Session cache of the last scan, so the list's Copies column and the details
  // dialog can both read it without rescanning. Cleared on demand, never saved:
  // it describes the disk at one moment, and a stale count is worse than none.
  std::unordered_map<std::wstring, std::vector<std::wstring>> m_dupeIndex;  // hash -> paths
  // Every hash the scan actually looked at, duplicated or not.  Without it a
  // hash missing from m_dupeIndex is ambiguous -- "scanned, exactly one file"
  // and "lives outside the folder that was scanned" would both read as one
  // copy, and the second is a claim the app has not earned.
  std::set<std::wstring> m_dupeScannedHashes;
  bool m_bDupeScanRun = false;   // distinguishes "no copies" from "never looked"
  // How many files on disk share this annotation's content.
  // 0 = unknown (no scan, or this preset was not under the scanned root).
  int  DuplicateCountFor(const PresetAnnotation& a) const;
  // Average of the rating observations, rounded; 0 when there are none.
  static int  AverageRating(const PresetAnnotation& a);
  // The rating to display: MDX12 average when observations exist, else the
  // preset file's own fRating, else 0.
  int  EffectiveRating(const wchar_t* filename, float fFileRating) const;
  void SetPresetRatingMDX(const wchar_t* filename, int value);
  void SetPresetRatingForFile(const wchar_t* filename, int value);
  static void AdoptHashIntoLegacyRatings(PresetAnnotation& a, const std::wstring& hash);
  void ResetUsageStats(const wchar_t* filenameOrNull);

  // ── Shader overrides (shader_overrides.h) ──
  //
  // The override's text is held HERE and never written into m_pState.
  // CState::Export writes m_szWarpShadersText / m_szCompShadersText straight
  // back into the .milk, so staging an override in state would silently bake
  // someone else's shader into the user's preset file the next time they saved
  // it.  Keeping state pristine makes that impossible rather than unlikely.
  // Where a resolved selection came from. Reported over IPC per slot, because
  // "the tag rule worked" and "the per-preset entry worked" are the two things
  // this feature is made of and the result alone cannot tell them apart.
  enum class OverrideSource { None, Rule, Preset };

  // ── Audio profile (audio_profile_store.h) ──
  // m_audioProfile is written ONLY by ApplyPendingAudioProfile on the render
  // thread and read by the three per-frame consumers on that same thread.
  // Resolution runs on the preset-load thread and communicates by name.
  AudioProfile        m_audioProfile;
  wchar_t             m_szPendingAudioProfile[128] = L"MDropDX12";
  std::atomic<bool>   m_bAudioProfilePending{ false };
  std::wstring        m_resolvedAudioProfile;
  OverrideSource      m_resolvedAudioSource = OverrideSource::None;
  wchar_t             m_szDefaultAudioProfile[128] = L"MDropDX12";

  struct ActiveShaderOverride {
    std::wstring name;
    std::string  warpText, compText;   // empty slot = keep the preset's own
    bool fromRule = false;             // false when applied by hand
    bool warpFailed = false, compFailed = false;
    std::wstring matchedTag;
    OverrideSource source = OverrideSource::None;
    bool IsActive() const { return !name.empty(); }
    void Clear() { *this = ActiveShaderOverride(); }
  };
  ActiveShaderOverride m_activeOverride;

  // The VFX profile this preset resolved to, and from where. Resolved
  // independently of the shader slot: a preset may take its shader from a
  // generic tag rule while naming its own profile.
  std::wstring   m_resolvedVFXProfile;
  OverrideSource m_resolvedVFXSource = OverrideSource::None;

  void ResolveShaderOverrideForPreset(CState* pState);
  PresetAnnotation* GetAnnotationForPreset(const wchar_t* filename,
                                          CState* pState, bool create);
  void ResolveAudioProfileForPreset(CState* pState);
  void ApplyPendingAudioProfile();
  bool ApplyOverrideToCurrentPreset(const std::wstring& name);  // ad hoc, no rule
  void RevertOverrideOnCurrentPreset();
  void RequestShaderRecompile();       // posts the render-thread recompile
  // True only for the state being rendered or loaded. Shader precompilation
  // (CompilePresetShadersToFile) builds unrelated presets and must compile
  // exactly what those files contain.
  bool OverrideAppliesTo(const CState* pState) const {
    return m_activeOverride.IsActive() &&
           (pState == m_pState || pState == m_pNewState);
  }

  // Usage tracking.  A preset counts as played once it has been on screen for
  // kUsageCountThresholdSec, reusing the threshold the startup-preset save
  // already applies: cycling through forty presets looking for one should not
  // log forty plays.  Seconds accumulate from load, including those first five,
  // but only for a preset that crossed the threshold.
  static constexpr float kUsageCountThresholdSec = 5.0f;
  float   m_fPresetUsageStart = 0;      // GetTime() when the running preset loaded
  bool    m_bPresetUsageCounted = false;
  // Set when the usage tick decided this preset must not be recorded (testing
  // mode, or an ignored directory).  Separate from m_bPresetUsageCounted, which
  // the tick also sets in that case purely to stop re-testing every frame --
  // and which FlushPresetUsage reads as "this play was counted, bank its
  // seconds".  Without this second flag the skip would suppress the play count
  // and then add the time anyway.
  bool    m_bPresetUsageSuppressed = false;
  wchar_t m_szUsagePresetFile[512] = {}; // full path of the preset being timed
  void TickPresetUsage();                // once per frame
  // Drop recorded alias paths that hash to a different preset than the
  // entry holding them. Returns how many were removed.
  int  RepairAnnotationPaths(int* pChecked = nullptr, int* pUnjudged = nullptr);
  void FlushPresetUsage();               // on preset change and at shutdown
  void BeginPresetUsage(const wchar_t* fullPath);

  // Preset lists: save/load named subsets of presets
  bool SavePresetList(const wchar_t* listName);  // saves current preset list to file
  bool LoadPresetList(const wchar_t* listPath);   // loads a preset list from file
  void GetPresetListDir(wchar_t* szDir, int nMax) const;  // preset_lists/ dir
  void EnumPresetLists(std::vector<std::wstring>& names) const;  // list available .txt files
  void AutoFlagPresetError(const wchar_t* filename, const std::wstring& errorMsg,
                           PresetErrorKind kind = PresetErrorKind::Shader);
  // Retract a shader error the preset has outgrown -- see the definition.
  void ClearPresetShaderError(const wchar_t* filename);
  // Import: parse annotations from an arbitrary presets.json file
  static std::unordered_map<std::wstring, PresetAnnotation> ParseAnnotationsFile(const wchar_t* path);
  // Scan loaded presets and build a map from fRatingThis (non-default ratings only)
  std::unordered_map<std::wstring, PresetAnnotation> ScanPresetsForRatings();

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
  void BuildPresetPath(int idx, wchar_t* szOut, int nMax) const;  // absolute path from m_presets[idx]
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
  FFT            m_fftShader;  // separate clean FFT for shader texture — no equalization, Hann³ window
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

  // True when nothing may change the preset on its own. Testing mode aside,
  // this is the condition that was copy-pasted at eight hard-cut sites.
  // The Preset Editor also holds it down: an auto-advance while someone is
  // typing into the live preset would throw their unsaved edits away.
  bool AutoPresetChangesAllowed() const {
    return !m_bPresetLockedByUser && !m_bPresetLockedByCode && !m_bTestingMode &&
           !m_bPresetEditorOpen.load(std::memory_order_relaxed);
  }
  // Returns true if the key was swallowed by testing mode.
  bool TestingModeHandleKey(unsigned int vk);
  void SetTestingMode(bool bOn);

  void AddNotification(wchar_t* szMsg);
  void AddNotificationAudioDevice();
  void AddNotification(wchar_t* szMsg, float time);
  void AddNotificationColored(wchar_t* szMsg, float time, DWORD color);
  void AddError(wchar_t* szMsg, float fDuration, int category = ERR_ALL, bool bBold = true);
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
  // Same comp/Image shader, built for a FLOAT32 render target. The .milk3 Image
  // pass draws into m_dx12ImageFeedback (FLOAT32) whenever the preset samples
  // its own previous Image ("self"), and a PSO whose RTVFormats[0] is the UNORM
  // backbuffer format may not be bound to that target — mismatched formats are
  // invalid D3D12 and, with no debug layer, corrupt or hang instead of erroring.
  ComPtr<ID3D12PipelineState> m_dx12CompFloatPSO;    // comp built for FLOAT32 RT
  ComPtr<ID3D12PipelineState> m_dx12FallbackWarpPSO; // default warp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12FallbackCompPSO; // default comp_ps.fx
  ComPtr<ID3D12PipelineState> m_dx12OldWarpPSO;      // previous preset warp (blend pass 0, no alpha)
  ComPtr<ID3D12PipelineState> m_dx12WarpBlendPSO;    // current preset warp (blend pass 1, alpha blend)
  ComPtr<ID3D12PipelineState> m_dx12OldCompPSO;      // previous preset comp (blend pass 0, no alpha)
  ComPtr<ID3D12PipelineState> m_dx12CompBlendPSO;    // current preset comp (blend pass 1, alpha blend)
  ComPtr<ID3D12PipelineState> m_dx12BlurPSO[2];      // [0] = horiz (blur1), [1] = vert (blur2)
  DX12Texture m_injectEffectTex;                     // back-buffer-sized copy for F11 inject post-process
  ComPtr<ID3D12PipelineState> m_pInjectEffectPSO;    // inject effect pixel shader PSO
  void RenderInjectEffect();                         // F11 inject effect post-process pass
  // Feedback-canvas ceiling (long edge, px; 0 = no limit). A per-preset
  // canvasMax may only reduce below this -- see EffectiveCanvasLimit.
  int m_nGlobalCanvasMax = 0;

  // min(global, per-preset). Never max(): a preset can only step the canvas
  // down, never raise it above the global ceiling.
  // Long-edge cap for the feedback canvas, resolved content-hash first.
  // Pass the incoming preset's hash when calling mid-apply, before the
  // state swap -- see the definition.
  int EffectiveCanvasLimit(const char* szHashOverride = nullptr) const;

  // The canvas long edge these presets were balanced at, and the pivot for the
  // damp mitigation. Below it the mitigation is inert, at it the multiplier is
  // exactly 1.0, above it the multiplier tracks how much per-frame transport
  // the bigger canvas has taken away. 1024 is where the measured sweep found
  // the runaway presets still stable (0.52 MP stable / 8.29 MP runaway).
  static const int kDampReferenceEdge = 1024;

  // What strength 1.0 on the dial means: remove at most this fraction of the
  // feedback per frame, in the limit of an infinitely large canvas. A range
  // for a user control, not a correction fitted to a preset -- the curve's
  // shape comes from the canvas, see EffectiveFeedbackDamp.
  //
  // 0.15 because it is the smallest value that puts BOTH multipliers ever
  // measured to work inside the dial, at opposite ends of it. At a 3840 px long
  // edge (lost = 0.7333):
  //
  //     0.97, which converged "Flexi - jellyfish jam"  -> strength 0.27, Gentle
  //     0.90, which collapsed "suksma - tetraxectsual"  -> strength 0.91, Maximum
  //
  // At 0.10 the second of those needed strength 1.36 and was simply
  // unreachable, which is the defect; the first sat at 0.41 and everything
  // useful crowded into the top of the range. Not higher than 0.15: at 0.20 the
  // top of the dial is a 15%/frame cut at that canvas, enough to visibly
  // shorten trails on a preset that was fine to begin with.
  //
  // Note what this is NOT evidence of. "Strength 1.0 does nothing to a runaway"
  // was never demonstrated -- the run that appeared to show it had a preset
  // whose loop never ignited (its strength-0 row read 0.056, already calm), so
  // 0.9267 was never tested against an ignited buffer. The case for 0.15 rests
  // on reachability of known-good values, not on a measured failure of 0.10.
  static constexpr float kDampMaxLoss = 0.15f;

  // The strengths offered in the UI, defined ONCE. The canvas-limit choices
  // are spelled out separately in three files and have to be kept in step by
  // hand; there is no reason to repeat that.
  static constexpr float kDampChoices[5] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
  static const wchar_t* DampChoiceLabel(int i);
  // Which entry of kDampChoices a stored strength corresponds to, so the
  // three UI surfaces that have to tick the right item do not each carry
  // their own float comparison. Anything unrecognised reads as Off.
  static int DampChoiceIndex(float strength);

  // A live, session-only feedback multiplier that beats the annotation and the
  // scratch-preset guard alike. Negative = no override.
  //
  // Exists so a mitigation can be tried on the running frame without editing a
  // preset, writing presets.json or relaunching -- which is what Shane asked
  // for, and is also the only way to measure the mechanism at all: the ruler
  // preset that makes the multiplier readable lives in a TEST folder, and a
  // TEST preset under testing mode deliberately has no identity to hang an
  // annotation on.
  float m_fDampOverride = -1.0f;

  // Per-frame feedback multiplier for the current preset, derived from the
  // canvas -- 1.0 means "do nothing", which is the answer for every preset
  // without a feedbackDamp annotation and for every canvas at or below the
  // reference edge. See the definition for why it is never a fitted constant.
  float EffectiveFeedbackDamp() const;

  // The damp strength resolved for the preset currently on screen, 0 = off.
  //
  // Cached rather than looked up per frame, because the lookup reads
  // m_presetAnnotations and EffectiveFeedbackDamp runs on the RENDER thread
  // once every frame, while the IPC and ToolWindow threads insert into that
  // same map. EffectiveCanvasLimit gets away with the live lookup by being
  // called only on a preset change or a resize; this would have been a
  // std::map read racing a std::map insert sixty times a second.
  //
  // The STRENGTH is cached and the multiplier is derived per frame, so a
  // window resize takes effect immediately without anything having to notice
  // the preset did not change.
  float m_fCurrentDampStrength = 0.0f;

  // Re-resolve m_fCurrentDampStrength. Call on a preset change (passing the
  // INCOMING preset's hash, since m_pState is still the outgoing one at that
  // point) and whenever the annotation is written.
  void RefreshCurrentDampStrength(const char* szHashOverride = nullptr);

  // Does the running preset's composite shader invert or fold the feedback it
  // samples? If so the damp will BRIGHTEN it rather than calm it -- see
  // comp_inversion.h. A hint for the user, never acted on: nothing here
  // enables, disables or reverses a mitigation on its own.
  bool CompShaderInvertsFeedbackNow() const;

  // The running preset's bare filename. The `wcsrchr(..., '\') ? +1 : whole`
  // dance appears in a dozen places; the UI needs it to ask "is the row I am
  // drawing the preset that is actually on screen?".
  const wchar_t* CurrentPresetFilename() const;

  // Bleed energy out of the feedback buffer: a full-screen black quad at
  // alpha = 1 - damp, drawn into VS[1] right after the warp mesh so the
  // shapes and waves that follow are NOT attenuated -- they are this frame's
  // new signal, not the accumulated history that is running away.
  void ApplyFeedbackDamp(ID3D12GraphicsCommandList* cmdList, float damp);
  // Long-edge clamp + 16-align for the MIRROR's own feedback canvas. The
  // mirror simulates into its own textures, so a canvasMax that only shrank
  // the primary left it running at 1920x1088 (2.09 MP) while the primary sat
  // at 432x768 (0.33 MP) — "Flexi - jellyfish jam.milk" still decayed to a
  // flat gradient on the panels with the window perfectly healthy (Shane,
  // 2026-08-23). Both sizing sites MUST call this: if they disagree the pipe
  // is recreated every frame, which TDRs.
  void ClampOrientCanvas(int& w, int& h) const;
  // Does the existing orient pipe have the textures the CURRENT preset mode
  // needs? classic wants imgFb[2]; milk3 additionally wants fbA[2]. Both the
  // render thread's "do I need to rebuild" test and EnsureOrientPipeline's
  // early-out must use this one function: when only the latter knew about the
  // mode, a classic -> .milk3 switch left the render thread thinking the pipe
  // was fine while the worker (barred from rebuilding it) skipped every frame,
  // and the mirrors froze on every .milk3 preset (2026-08-23).
  bool OrientPipeReadyForMode() const;
  // The canvas limit the orient pipe was BUILT with, latched at creation and
  // deliberately not tracked live. Resizing an existing orient pipe hangs the
  // GPU: ReleaseOrientPipeline drops the textures but DXContext::AllocateRtv is
  // a bump allocator with no free path, so each recreate burns ~10 RTV slots
  // below DXC_MIRROR_RTV_BASE until the allocator rewinds and the new RTVs
  // alias live ones. Deterministic TDR (0x887A0006) ~4 s after stepping off a
  // preset whose canvasMax differed, 2026-08-23; with the size held constant
  // the same transition is clean. Latching means a preset change never resizes
  // the pipe — the mirror adopts the current limit when the pipe is next built
  // (mirror enable, panel/orientation change, epoch recreate).
  int m_nOrientCanvasLimit = 0;

  // The limit the live canvas was actually allocated for. Compared against
  // EffectiveCanvasLimit() after a preset change, because the canvas is sized
  // in AllocateMyDX9Stuff and loading a preset does not otherwise resize it.
  // -1 = never allocated.
  int m_nCanvasLimitApplied = -1;

  // Write a per-preset canvas limit by filename (0 clears it). Shared by the
  // Presets context menu and the Annotations combo so the two surfaces cannot
  // drift. Rebuilds the canvas only when the affected preset is the one
  // playing -- setting a limit on a preset you are not watching must not
  // resize the live canvas.
  void SetPresetCanvasMaxByFile(const wchar_t* filenameOnly, int px);

  // Per-preset damp strength, 0..1 (0 clears). Companion to the above --
  // the two mitigations are independent and either, both, or neither may
  // be set on a preset.
  void SetPresetFeedbackDampByFile(const wchar_t* filenameOnly, float strength);

  // Live search predicate for the Annotations list. Pure -- no UI state -- so
  // it is testable over IPC and safe to call from the ToolWindow thread.
  // Substring by default; glob once the query contains * or ?.
  bool AnnotationMatches(const PresetAnnotation& a, const wchar_t* query) const;

  // Full path that exists on disk for this annotation, or empty if the preset
  // is gone. Consults the recorded `paths` as well as the current preset dir --
  // annotations outlive directory changes.
  std::wstring ResolveAnnotationPath(const PresetAnnotation& a) const;

  // Drop annotations whose preset can no longer be found. Returns the count.
  // True only when the entry NAMES locations and none of them exist. An entry
  // with no recorded path is not missing -- it is unlocated, which is not the
  // same thing and must never be deleted on that basis.
  bool IsAnnotationKnownMissing(const PresetAnnotation& a) const;

  int RemoveMissingAnnotations();

  // Canvas-limit / metric / annotation-query IPC. Split out of LaunchMessage:
  // that function is one long else-if chain and these tipped it past MSVC's
  // block nesting limit (C1061). Returns true when it handled the message.
  bool HandleCanvasIPC(const wchar_t* sMessage);

  DX12Texture m_canvasSrcTex;                        // back-buffer copy sampled by the canvas metric reduction
  DX12Texture m_canvasReduceTex;                     // 8x8 reduction target for the canvas metric
  ComPtr<ID3D12PipelineState> m_pCanvasReducePSO;    // 8x8 frame-reduction PSO
  CanvasMetric m_canvasMetric;                       // Phase 1 instrumentation: measures the presented frame
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
  bool m_bHasBufferC = false;                        // true when preset has a Buffer C shader
  bool m_bHasBufferD = false;                        // true when preset has a Buffer D shader
  bool m_bShadertoyMode = false;                     // true when a .milk3 Shadertoy preset is active
  int  m_nShadertoyStartFrame = 0;                   // frame at which Shadertoy mode was activated (for iFrame=0)
  ComPtr<ID3D12PipelineState> m_dx12BufferAPSO;      // Buffer A pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferBPSO;      // Buffer B pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferCPSO;      // Buffer C pixel shader PSO
  ComPtr<ID3D12PipelineState> m_dx12BufferDPSO;      // Buffer D pixel shader PSO
  DX12Texture m_dx12FeedbackB[2];                    // ping-pong feedback buffers for Buffer B (FLOAT32)
  DX12Texture m_dx12FeedbackC[2];                    // ping-pong feedback buffers for Buffer C (FLOAT32)
  DX12Texture m_dx12FeedbackD[2];                    // ping-pong feedback buffers for Buffer D (FLOAT32)
  std::atomic<int> m_nRecompileResult{0};            // 0=idle, 1=pending, 2=done-ok, 3=done-fail
  void CopyBackbufferToFeedback();                   // capture comp output for next frame's feedback (single-pass)
  void RenderFrameShadertoy(ID3D12GraphicsCommandList* cmdList);  // Shadertoy pipeline (skip warp/blur/shapes)
  // See also RenderMilk3ImageToMirror / BlitMainToMirror (display outputs)
  UINT m_warpMainTexSlot = 0;                         // t-register for sampler_main in warp PS
  UINT m_compMainTexSlot = 0;                         // t-register for sampler_main in comp PS
  UINT m_oldWarpMainTexSlot = 0;                      // t-register for sampler_main in old warp PS
  UINT m_oldCompMainTexSlot = 0;                      // t-register for sampler_main in old comp PS
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
  //
  // The parameter set itself is in video_effect_params.h. It was nested in
  // Engine, which made every consumer say Engine::VideoEffectParams and put a
  // ~1,900 line header in the way of touching a parameter set at all.
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
  //
  // Every profile lives in ONE file, vfxprofiles.json, keyed by name -- not a
  // directory of small files, and not a live-state file that saves itself as
  // you drag a slider. Nothing about video effects is written unless a profile
  // is explicitly saved, and nothing is restored at startup unless "Load on
  // startup" names one. Parameters otherwise begin at their defaults.
  //
  // A rewrite re-emits members this build does not recognise, so the format
  // can gain sections later without an older build erasing them.
  //
  // m_videoFXSaved is the baseline the Save button's red state compares
  // against: the parameters as they were when the current profile was last
  // saved or loaded. Live != baseline means there is something to save.
  wchar_t m_szCurrentVFXProfile[MAX_VFX_PROFILE_NAME] = {};  // loaded profile NAME (empty = none)

  // ── Scoped VFX ──
  //
  // A profile applied because the running preset (or one of its tags) asked
  // for it. It is SCOPED: the VFX state from before the preset is snapshotted
  // and restored when the preset is left, so a preset cannot permanently move
  // settings the user did not change.
  //
  // One thing deliberately does NOT happen when one of these is applied:
  //   * m_szCurrentVFXProfile is not set -- SaveVideoFXOnExit writes the
  //     CURRENT profile back to disk, so a rule firing silently would make the
  //     app overwrite a profile the user never chose.
  //
  // If the state is edited during a scope the edit is kept and the scope is
  // marked dirty; the restore then waits for a Keep/Discard answer rather than
  // silently reverting a deliberate change.
  bool           m_bVFXScopeActive = false;
  bool           m_bVFXScopeDirty  = false;
  VFXProfileData m_vfxScopeBaseline;
  wchar_t        m_szVFXScopeProfile[MAX_VFX_PROFILE_NAME] = {};

  void PushScopedVFXProfile(const std::wstring& name);
  void PopScopedVFXProfile();
  // Answer a pending scoped edit. Keep writes the live fx into the scoped
  // profile; either way the baseline is then restored. This is the ONE path
  // that writes vfxprofiles.json for this feature, and both the prompt's
  // buttons and VFX_SCOPED_KEEP go through it so there is a single behaviour.
  void AnswerScopedVFX(bool bKeep);
  // Exit path: restores the baseline even when dirty. There is no opportunity
  // to ask at shutdown, and discarding the edit is far better than letting
  // Save-on-exit write it into whatever profile happens to be current.
  void ForceRestoreScopedVFX();
  bool    m_bEnableVFXStartup = false;
  wchar_t m_szVFXStartup[MAX_VFX_PROFILE_NAME] = {};
  // Off by default: saving a profile is an explicit act, and the Save Profile
  // button goes red while there is something to save. Turning this on says
  // "and also write it back when the program exits" -- program exit, not the
  // Video Effects window closing, which happens all the time.
  bool    m_bEnableVFXSaveOnExit = false;
  VideoEffectParams m_videoFXSaved;              // baseline for the dirty check
  // The store itself is VFXProfileStore (vfx_profile_store.h) -- the file, the
  // schema and the importers, with no Engine dependency. Call it directly for
  // anything that is purely about the file: m_vfxProfiles.Names(), .Delete(),
  // .Exists(), .Import().
  //
  // Only the two operations below stay here, because they are the ones that
  // bridge the store and the engine's live state.
  VFXProfileStore m_vfxProfiles;

  // Call once at startup, before anything touches the store.
  void InitVFXProfileStore();

  // Live parameters as a profile's worth of data, and back. Defined in
  // engine_video_effects_ui.cpp, beside the rest of the store bridge.
  VFXProfileData CaptureVFXProfile() const;
  void           ApplyVFXProfile(const VFXProfileData& d);

  bool    SaveVideoFXProfile(const wchar_t* name);
  // Also records the name in m_szCurrentVFXProfile. Sections the stored
  // profile does not carry leave the live values alone -- see
  // VFXProfileStore::Load.
  bool    LoadVideoFXProfile(const wchar_t* name);

  // Called once on the way out. Writes the loaded profile back if the user
  // asked for that with "Save on exit"; does nothing otherwise.
  void    SaveVideoFXOnExit();

  // Are there parameter changes not written to the current profile?
  // With no profile loaded there is nothing to be dirty against, so false.
  // Defined in engine_video_effects_ui.cpp, where the tunable table is in
  // scope -- it covers the render tunables as well as the effect parameters.
  bool    IsVideoFXDirty() const;

  // Snapshot live values as the clean baseline. Call after saving or loading a
  // profile; one function so the two halves cannot be updated separately.
  void    MarkVideoFXSaved();

  // Call after ANY change to m_videoFX: persists live state and repaints the
  // Save button so its red state tracks reality.
  void    OnVideoFXChanged();

  // VFX Profile Picker Window
  class CustomShadersWindow* m_pCustomShadersWindow = nullptr;
  void OpenCustomShadersWindow();
  void CloseCustomShadersWindow();

  class VFXProfileWindow* m_pVFXProfileWindow = nullptr;
  void OpenVFXProfileWindow();
  void CloseVFXProfileWindow();

  // Raised when a preset is left after its scoped VFX profile was edited.
  class VFXKeepPromptWindow* m_pVFXKeepPromptWindow = nullptr;
  void OpenVFXKeepPrompt(const std::wstring& profileName);
  void CloseVFXKeepPrompt();

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
  // Warp-mesh indexed submission scratch (see DrawWarpMeshIndexed).
  // m_warpVertsStaged holds one cDecay-tinted copy of m_verts; m_warpIdx16All is the
  // full triangle list as 16-bit indices, built once; m_warpIdx16 is the culled subset.
  MYVERTEX* m_warpVertsStaged = nullptr;
  UINT16*   m_warpIdx16 = nullptr;
  UINT16*   m_warpIdx16All = nullptr;
  int       m_warpIdx16AllCount = 0;
  void DrawWarpMeshIndexed(const MYVERTEX* srcVerts, DWORD cDecay,
                           bool bCullTiles, bool bFlipCulling,
                           ID3D12GraphicsCommandList* cmdList);
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
  int                  m_nPresetNameAnimProfile = -3;  // -1 = disabled, -2 = random, -3 = simple, 0+ = profile

  // Simple preset name HUD display (non-animated, fixed font size)
  wchar_t              m_szPresetNameDisplay[512] = {};
  float                m_fPresetNameShowUntil = -1.0f;

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
  void        GenWarpPShaderText(char* szShaderText, bool bWrap);
  void        GenCompPShaderText(char* szShaderText, float hue_shader, bool bBrighten, bool bDarken, bool bSolarize, bool bInvert);

  //====[ 2. methods added: ]=====================================================================================

  void RenderFrame(int bRedraw);
  void DX12_RenderWarpAndComposite();
  void DX12_DrawWave(float* fL, float* fR);
  void DX12_DrawSprites();
  // Overrides = mirror sim contexts drawing with their own states; null =
  // the primary's m_pState/m_pOldState (default, unchanged behavior).
  void DX12_DrawCustomShapes(CState* pNewOverride = nullptr, CState* pOldOverride = nullptr);
  void DX12_DrawCustomWaves(CState* pNewOverride = nullptr, CState* pOldOverride = nullptr);
  void AlignWave(int nSamples);

  void        DrawTooltip(wchar_t* str, int xR, int yB);
  void        RandomizeBlendPattern();
  // Recomputes the frozen .milk2 wipe field; needed at load AND after a
  // device teardown, which reallocates m_vertinfo uninitialised.
  void        ApplyMilk2BlendPattern();
  // Fills m_vertinfo with the measured MD3 PRO wipe field for a named
  // pattern. Returns false if that pattern has no measured analytic form,
  // in which case the caller falls back to RandomizeBlendPattern().
  bool        ComputeMilk2BlendField(const char* pattern, float bandCoord,
                                     int direction);
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
  void        ParseEmbeddedSprites(const std::string& buf);
  bool        LoadEmbeddedSpritesFromFile(const wchar_t* szPath);
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
  void    SendTrackInfoToMDropDX12Remote();
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
  // cmdList: optional (null = main frame list). Used to re-draw messages onto mirror RTs.
  void		ShowSongTitleAnim(/*IDirect3DTexture9* lpRenderTarget,*/ int w, int h, float fProgress, int supertextIndex,
                            ID3D12GraphicsCommandList* cmdList = nullptr);
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
  // targetLayer: -1=all, 0=classic behind-text, 1=front,
  //              10=milk2 in-back (layer 0), 12=milk2 merge (layers 2–4)
  void		DrawUserSprites(int targetLayer = -1, ID3D12GraphicsCommandList* cmdList = nullptr);
  // pState/warpVerts overrides: mirror sim ctx (blend alpha comes from ITS
  // wipe mesh at its aspect); nulls = primary members.
  void        UpdateCompMeshBlendColors(const DWORD cShade[4],
                                        CState* pState = nullptr,
                                        const MYVERTEX* warpVerts = nullptr);
  void        DrawCompMesh(bool bCullTiles, bool bFlipCulling, ID3D12GraphicsCommandList* cmdList = nullptr);
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
  // Registry of all live ToolWindows (for iteration — self-register in ctor, deregister in dtor)
  std::vector<ToolWindow*> m_toolWindows;

  // Template helpers for standard Open/Close pattern
  template<typename T>
  void OpenToolWindow(std::unique_ptr<T>& ptr) {
    if (!ptr) ptr = std::make_unique<T>(this);
    ptr->Open();
  }
  template<typename T>
  void CloseToolWindow(std::unique_ptr<T>& ptr) {
    if (ptr) ptr->Close();
  }

  // ToolWindow subclass instances (each runs on its own thread)
  std::unique_ptr<SettingsWindow>       m_settingsWindow;
  int         m_nSettingsFontSize = -20;     // Shared font size for all tool windows (negative = pixel height)
  std::unique_ptr<DisplaysWindow>       m_displaysWindow;
  std::unique_ptr<SongInfoWindow>       m_songInfoWindow;
  std::unique_ptr<HotkeysWindow>        m_hotkeysWindow;
  std::unique_ptr<MidiWindow>           m_midiWindow;
  std::unique_ptr<PresetsWindow>        m_presetsWindow;
  std::unique_ptr<AnnotationsWindow>    m_annotationsWindow;
  std::unique_ptr<SpritesWindow>        m_spritesWindow;
  std::unique_ptr<MessagesWindow>       m_messagesWindow;
  std::unique_ptr<ButtonBoardWindow>    m_boardWindow;
  std::unique_ptr<ShaderImportWindow>   m_shaderImportWindow;
  std::unique_ptr<ScriptWindow>         m_scriptWindow;
  std::unique_ptr<RemoteWindow>         m_remoteWindow;
  std::unique_ptr<VisualWindow>         m_visualWindow;
  std::unique_ptr<ColorsWindow>         m_colorsWindow;
  std::unique_ptr<ControllerWindow>     m_controllerWindow;
  std::unique_ptr<WelcomeWindow>        m_welcomeWindow;
  std::unique_ptr<TextAnimWindow>       m_textAnimWindow;
  std::unique_ptr<WorkspaceLayoutWindow> m_workspaceLayoutWindow;
  std::unique_ptr<PresetEditorWindow>   m_presetEditorWindow;

  // Open/Close — standard pattern uses template; non-standard kept as declarations
  void OpenSettingsWindow()      { OpenToolWindow(m_settingsWindow); }
  void CloseSettingsWindow()     { CloseToolWindow(m_settingsWindow); }
  void OpenDisplaysWindow()      { OpenToolWindow(m_displaysWindow); }
  void CloseDisplaysWindow()     { CloseToolWindow(m_displaysWindow); }
  void OpenSongInfoWindow()      { OpenToolWindow(m_songInfoWindow); }
  void CloseSongInfoWindow()     { CloseToolWindow(m_songInfoWindow); }
  void OpenHotkeysWindow()       { OpenToolWindow(m_hotkeysWindow); }
  void CloseHotkeysWindow()      { CloseToolWindow(m_hotkeysWindow); }
  void OpenMidiWindow()          { OpenToolWindow(m_midiWindow); }
  void CloseMidiWindow()         { CloseToolWindow(m_midiWindow); }
  void OpenPresetsWindow()       { OpenToolWindow(m_presetsWindow); }
  void ClosePresetsWindow()      { CloseToolWindow(m_presetsWindow); }
  void OpenAnnotationsWindow()   { OpenToolWindow(m_annotationsWindow); }
  void CloseAnnotationsWindow()  { CloseToolWindow(m_annotationsWindow); }
  void OpenPresetEditorWindow()  { OpenToolWindow(m_presetEditorWindow); }
  void ClosePresetEditorWindow() { CloseToolWindow(m_presetEditorWindow); }
  void OpenSpritesWindow()       { OpenToolWindow(m_spritesWindow); }
  void CloseSpritesWindow()      { CloseToolWindow(m_spritesWindow); }
  void OpenBoardWindow()         { OpenToolWindow(m_boardWindow); }
  void CloseBoardWindow()        { CloseToolWindow(m_boardWindow); }
  void OpenShaderImportWindow()  { OpenToolWindow(m_shaderImportWindow); }
  void CloseShaderImportWindow() { CloseToolWindow(m_shaderImportWindow); }
  void OpenScriptWindow()        { OpenToolWindow(m_scriptWindow); }
  void CloseScriptWindow()       { CloseToolWindow(m_scriptWindow); }
  void OpenRemoteWindow()        { OpenToolWindow(m_remoteWindow); }
  void CloseRemoteWindow()       { CloseToolWindow(m_remoteWindow); }
  void OpenVisualWindow()        { OpenToolWindow(m_visualWindow); }
  void CloseVisualWindow()       { CloseToolWindow(m_visualWindow); }
  void OpenColorsWindow()        { OpenToolWindow(m_colorsWindow); }
  void CloseColorsWindow()       { CloseToolWindow(m_colorsWindow); }
  void OpenControllerWindow()    { OpenToolWindow(m_controllerWindow); }
  void CloseControllerWindow()   { CloseToolWindow(m_controllerWindow); }
  void OpenWelcomeWindow()       { OpenToolWindow(m_welcomeWindow); }
  void CloseWelcomeWindow()      { CloseToolWindow(m_welcomeWindow); }
  void OpenTextAnimWindow()      { OpenToolWindow(m_textAnimWindow); }
  void CloseTextAnimWindow();    // non-standard: resets after close
  void OpenMessagesWindow()      { OpenToolWindow(m_messagesWindow); }
  void CloseMessagesWindow();    // non-standard: resets after close
  void OpenWorkspaceLayoutWindow() { OpenToolWindow(m_workspaceLayoutWindow); }
  void CloseWorkspaceLayoutWindow() { CloseToolWindow(m_workspaceLayoutWindow); }

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
  void        DX12_DrawMotionVectors();

  bool        LoadShaders(PShaderSet* sh, CState* pState, bool bTick, bool bCompileOnly);
  void        UvToMathSpace(float u, float v, float* rad, float* ang);
  void        ApplyShaderParams(CShaderParams* p, LPD3DXCONSTANTTABLE pCT, CState* pState);
  void        RestoreShaderParams();
  // bNoPrimaryFallback: when true, never fall back to primary m_dx12Feedback* (orient pipe).
  void        BuildBindingSlots(CShaderParams* params, const DX12Texture& vsTex, UINT outSlots[32], const DX12Texture* feedbackTex = nullptr, const DX12Texture* imageFeedbackTex = nullptr, const DX12Texture* bufferBTex = nullptr, const DX12Texture* bufferCTex = nullptr, const DX12Texture* bufferDTex = nullptr, bool bNoPrimaryFallback = false);
  // ── Binding-slot snapshots (DIAG_BINDINGS over the pipe) ─────────────────
  // The primary fills a per-frame descriptor block through
  // UpdatePerFrameBindings; the mirror rebuilds its own blocks every frame at
  // m_orientPipe.bindBase from the SAME CShaderParams. When a disk texture
  // resolves on one path and lands on m_fallbackTexture on the other, nothing
  // in any existing log shows it — the fallback is bound silently. Record what
  // each path actually built so the two can be diffed side by side.
  //
  // Written under m_mirrorEngineMutex (both the primary frame and the mirror
  // record hold it), read unsynchronised by the IPC thread — diagnostic only,
  // same contract as DIAG_MIRRORS.
  struct BindSnapshot {
    UINT slots[32];       // resolved SRV heap index per t-register
    UINT bindingSrv[32];  // params->m_texture_bindings[i].dx12SrvIndex
    int  texcode[32];     // tex_code per t-register
    bool valid;
  };
  enum { BINDSNAP_WARP = 0, BINDSNAP_COMP, BINDSNAP_OLDWARP, BINDSNAP_OLDCOMP,
         BINDSNAP_COUNT };
  BindSnapshot m_bindSnapPrimary[BINDSNAP_COUNT] = {};
  BindSnapshot m_bindSnapMirror[BINDSNAP_COUNT] = {};
  static void CaptureBindSnapshot(BindSnapshot& dst, const CShaderParams* params,
                                  const UINT slots[32]);

  // Live fallback-texture swap requested over the pipe (SET_FALLBACK_TEX).
  // Building the texture uses the shared upload command list and the main
  // queue, so the IPC thread only parks the request — MyRenderFn applies it.
  std::atomic<bool> m_bFallbackTexRefreshPending{ false };
  int               m_nPendingFallbackStyle = 0;
  wchar_t           m_szPendingFallbackFile[MAX_PATH] = {};

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