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

#ifndef GEISS_TEXTURE_MANAGER
#define GEISS_TEXTURE_MANAGER 1

#define NUM_TEX 16

#ifdef _DEBUG
#define D3D_DEBUG_INFO  // declare this before including d3d9.h
#endif
#include <d3d9.h>
#include "../ns-eel2/ns-eel.h"
#include "md_defines.h"
#include "dx12helpers.h"  // DX12Texture

class DXContext;  // forward declaration

#define TEXMGR_ERROR_MASK                 0x0F
#define TEXMGR_ERR_SUCCESS                0
#define TEXMGR_ERR_BAD_INDEX              1

/*
#define TEXMGR_ERR_OPENING                2
#define TEXMGR_ERR_IMAGE_NOT_24_BIT       3
#define TEXMGR_ERR_IMAGE_TOO_LARGE        4
#define TEXMGR_ERR_CREATESURFACE_FAILED   5
#define TEXMGR_ERR_LOCKSURFACE_FAILED     6
#define TEXMGR_ERR_CORRUPT_JPEG           7
*/

#define TEXMGR_ERR_FORMAT                 8
#define TEXMGR_ERR_BADFILE                9
#define TEXMGR_ERR_OUTOFMEM               10
#define TEXMGR_WARNING_MASK               0xF0
#define TEXMGR_WARN_ERROR_IN_INIT_CODE    0x10
#define TEXMGR_WARN_ERROR_IN_REG_CODE     0x20

typedef struct {
  LPDIRECT3DTEXTURE9     pSurface;
  DX12Texture            dx12Surface;          // DX12 GPU texture
  ComPtr<ID3D12Resource> dx12UploadBuf;        // upload buffer (kept alive until KillTex)
  int                    img_w, img_h;
  /*
int                    tex_w, tex_h;
float                  scale_x, scale_y;  // the factors by which the original image was squished to become (img_w x img_h) texels in size.
DDPIXELFORMAT          ddpf;
  */
  wchar_t				   szFileName[512];
  float				   fStartTime;
  int                    nStartFrame;
  int                    nUserData;

  // Retirement state (#225). A sprite that ends itself mid-frame cannot be
  // torn down where it is noticed -- see RetireTex. Marked here, collected a
  // frame or two later once the GPU has passed killFence.
  //
  // The slot deliberately stays OCCUPIED while this is set: pSurface is what
  // LaunchSprite reads to find a free slot, so leaving it non-null is what
  // stops the slot being reloaded -- and its descriptor rewritten -- inside
  // the very window the retirement exists to protect.
  bool                   bPendingKill;
  UINT64                 killFence;
  // MD3 file-level SpriteSX/SY. Per-frame sx/sy are 1.0-based multipliers on these.
  float                  base_x;
  float                  base_y;
  float                  base_sx;
  float                  base_sy;

  // stuff for expressions:
  char            m_szExpr[8192];         // for expression eval
  NSEEL_CODEHANDLE				m_codehandle;	        // for expression eval
  // input variables for expression eval
  double* var_time, * var_frame, * var_fps, * var_progress;
  double* var_bass, * var_bass_att, * var_mid, * var_mid_att, * var_treb, * var_treb_att;
  // output variables for expression eval
  double* var_x, * var_y;
  double* var_sx, * var_sy, * var_rot, * var_flipx, * var_flipy;
  double* var_r, * var_g, * var_b, * var_a;
  double* var_blendmode;
  double* var_repeatx, * var_repeaty;
  double* var_done, * var_burn;
  double* var_layer;  // 0 = behind text (default), 1 = on top of text
  NSEEL_VMCTX	tex_eel_ctx;
}
td_tex;

class texmgr {
public:
  texmgr();
  ~texmgr();

  // members
  void Init();
  void InitDX12(DXContext* lpDX);
  int  LoadTex(wchar_t* szFilename, int iSlot, char* szInitCode, char* szCode, float time, int frame, unsigned int ck);
  int  LoadTexFromPixels(const void* pixels, UINT w, UINT h, int iSlot, char* szInitCode, char* szCode, float time, int frame);
  void KillTex(int iSlot);

  // Deferred KillTex, for callers that cannot safely release right now (#225).
  //
  // KillTex releases the texture and its upload buffer and writes a null
  // descriptor into the LIVE shader-visible SRV heap. Doing that while a
  // submitted command list still references the slot is a use-after-free and
  // an illegal concurrent descriptor write -- it is the same edge that hung
  // the GPU in #222, and DrawUserSprites reaches it from INSIDE the draw loop,
  // with the command list open, which is strictly worse than the #222 site.
  //
  // fence is the value the frame being recorded will signal. Once the GPU has
  // passed it, nothing in flight can still name this slot, and CollectRetired
  // performs the real KillTex. Marking is idempotent: the earliest fence wins,
  // so a slot marked on two successive frames is not held hostage by the later
  // one.
  void RetireTex(int iSlot, UINT64 fence);

  // Release everything whose fence the GPU has passed. Once per frame, render
  // thread. Cheap: NUM_TEX is 16 and the common case is no pending slots.
  void CollectRetired(UINT64 completedFence);

  // Is this slot live -- present and not already on its way out? Anything that
  // draws or reports a sprite must ask this rather than testing pSurface, or a
  // retiring sprite keeps drawing until its fence clears.
  bool IsLive(int iSlot) const {
    return iSlot >= 0 && iSlot < NUM_TEX &&
           m_tex[iSlot].pSurface && !m_tex[iSlot].bPendingKill;
  }

  void Finish();

  // Final, device-free release of the GPU objects only -- for process exit.
  // NOT a substitute for Finish(): that goes through KillTex(), which writes
  // null descriptors into the live SRV heap through m_lpDX12, and m_lpDX12 is
  // never nulled when the DXContext is deleted. By exit time it dangles, so
  // Finish() would sail past its own non-null guard into freed memory. This
  // touches nothing but the ComPtrs. See #131.
  void ReleaseGpuResources();

  // data
  td_tex          m_tex[NUM_TEX];

protected:
  // members
  //bool TryCreateDDrawSurface(int iSlot, int w, int h);
  void FreeVars(int iSlot);
  void FreeCode(int iSlot);
  void RegisterBuiltInVariables(int iSlot);
  bool RunInitCode(int iSlot, char* szInitCode);
  bool RecompileExpressions(int iSlot);
  void StripLinefeedCharsAndComments(char* src, char* dest);

  // data
  DXContext*         m_lpDX12 = nullptr;
  UINT               m_srvRegionStart = UINT_MAX; // base of pre-allocated SRV region for NUM_TEX slots
};

#endif