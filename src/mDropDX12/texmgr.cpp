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

#include "texmgr.h"
#include "../ns-eel2/ns-eel.h"
#include "support.h"
#include "engine.h"
#include "utility.h"
#include "dxcontext.h"
#include <wincodec.h>
#include <vector>
using namespace mdrop;

texmgr::texmgr() {}

texmgr::~texmgr() {
  // DO NOT RELEASE OR DELETE m_lpDD; CLIENT SHOULD DO THIS!
}

void texmgr::Finish() {
  for (int i = 0; i < NUM_TEX; i++) {
    KillTex(i);
    if (m_tex[i].tex_eel_ctx) {
      NSEEL_VM_free(m_tex[i].tex_eel_ctx);
      m_tex[i].tex_eel_ctx = nullptr;
    }
  }

  // DO NOT RELEASE OR DELETE m_lpDD; CLIENT SHOULD DO THIS!
}

void texmgr::Init(LPDIRECT3DDEVICE9 lpDD) {
  m_lpDD = lpDD;

  for (int i = 0; i < NUM_TEX; i++) {
    m_tex[i].pSurface = NULL;
    m_tex[i].dx12Surface.Reset();
    m_tex[i].dx12UploadBuf.Reset();
    m_tex[i].szFileName[0] = 0;
    m_tex[i].m_codehandle = NULL;
    m_tex[i].m_szExpr[0] = 0;
    m_tex[i].tex_eel_ctx = NSEEL_VM_alloc();
  }
}

void texmgr::InitDX12(DXContext* lpDX) {
  m_lpDX12 = lpDX;

  // Pre-allocate 17 SRV slots per texture slot (1 SRV + 16 binding block) to avoid
  // leaking descriptor heap slots on every preset texture load.
  if (!lpDX || !lpDX->m_device) return;
  m_srvRegionStart = lpDX->m_nextFreeSrvSlot;

  for (int i = 0; i < NUM_TEX; i++) {
    // Allocate 1 SRV slot (initialized to null)
    D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = lpDX->AllocateSrvCpu();
    m_tex[i].dx12Surface.srvIndex = lpDX->m_nextFreeSrvSlot;
    lpDX->m_device->CopyDescriptorsSimple(1, srvCpu,
        lpDX->GetSrvCpuHandleAt(lpDX->m_nullTexture.srvIndex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    lpDX->AllocateSrvGpu(); // bump

    // Allocate 16 binding block slots (all initialized to null)
    m_tex[i].dx12Surface.bindingBlockStart = lpDX->m_nextFreeSrvSlot;
    for (UINT s = 0; s < DXContext::BINDING_BLOCK_SIZE; s++) {
      D3D12_CPU_DESCRIPTOR_HANDLE dst = lpDX->AllocateSrvCpu();
      lpDX->m_device->CopyDescriptorsSimple(1, dst,
          lpDX->GetSrvCpuHandleAt(lpDX->m_nullTexture.srvIndex),
          D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
      lpDX->AllocateSrvGpu(); // bump
    }
  }
}

int texmgr::LoadTex(wchar_t* szFilename, int iSlot, char* szInitCode, char* szCode, float time, int frame, unsigned int ck) {
  if (iSlot < 0) return TEXMGR_ERR_BAD_INDEX;
  if (iSlot >= NUM_TEX) return TEXMGR_ERR_BAD_INDEX;

  // first, if this texture is already loaded, just add another instance.
  bool bTextureInstanced = false;
  {
    for (int x = 0; x < NUM_TEX; x++)
      if (m_tex[x].dx12Surface.IsValid() && _wcsicmp(m_tex[x].szFileName, szFilename) == 0) {
        m_tex[iSlot].pSurface = m_tex[x].pSurface;
        // Share the GPU resource (refcounted) but preserve this slot's pre-allocated SRV indices
        m_tex[iSlot].dx12Surface.resource = m_tex[x].dx12Surface.resource;
        m_tex[iSlot].dx12Surface.width    = m_tex[x].dx12Surface.width;
        m_tex[iSlot].dx12Surface.height   = m_tex[x].dx12Surface.height;
        m_tex[iSlot].dx12Surface.depth    = m_tex[x].dx12Surface.depth;
        m_tex[iSlot].dx12Surface.format   = m_tex[x].dx12Surface.format;
        m_tex[iSlot].dx12Surface.currentState = m_tex[x].dx12Surface.currentState;
        // srvIndex and bindingBlockStart are preserved from pre-allocation
        m_tex[iSlot].dx12UploadBuf = m_tex[x].dx12UploadBuf;
        m_tex[iSlot].img_w = m_tex[x].img_w;
        m_tex[iSlot].img_h = m_tex[x].img_h;
        wcscpy(m_tex[iSlot].szFileName, szFilename);
        m_tex[iSlot].m_szExpr[0] = 0;

        // Write a new SRV at this slot's pre-allocated descriptor and update its binding block
        if (m_lpDX12 && m_tex[iSlot].dx12Surface.srvIndex != UINT_MAX) {
          D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
          srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
          srvDesc.Format            = m_tex[x].dx12Surface.format;
          srvDesc.ViewDimension     = D3D12_SRV_DIMENSION_TEXTURE2D;
          srvDesc.Texture2D.MipLevels = 1;
          m_lpDX12->m_device->CreateShaderResourceView(
              m_tex[iSlot].dx12Surface.resource.Get(), &srvDesc,
              m_lpDX12->GetSrvCpuHandleAt(m_tex[iSlot].dx12Surface.srvIndex));
          m_lpDX12->UpdateBindingBlockTexture(
              m_tex[iSlot].dx12Surface.bindingBlockStart,
              m_tex[iSlot].dx12Surface.srvIndex);
        }

        bTextureInstanced = true;
        break;
      }
  }

  if (!bTextureInstanced) {
    // Free old resources:
    KillTex(iSlot);

    wcscpy(m_tex[iSlot].szFileName, szFilename);

    if (!m_lpDX12 || !m_lpDX12->m_device || !m_lpDX12->m_commandList) {
      { wchar_t dbg[256]; swprintf(dbg, 256, L"LoadTex: EARLY EXIT dx12=%p dev=%p cmdList=%p",
        m_lpDX12, m_lpDX12 ? m_lpDX12->m_device.Get() : nullptr,
        m_lpDX12 ? m_lpDX12->m_commandList.Get() : nullptr); DebugLogW(dbg, LOG_WARN); }
      return TEXMGR_ERR_BADFILE;
    }

    auto* dev = m_lpDX12->m_device.Get();
    auto* cmdList = m_lpDX12->m_commandList.Get();

    // Ensure COM is initialized on this thread for WIC
    // (render thread may not have COM initialized; S_FALSE means already initialized — both are fine)
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool bComOwner = (hrCom == S_OK); // only uninitialize if we were the one who initialized

    { wchar_t dbg[512]; swprintf(dbg, 512, L"LoadTex: CoInitializeEx hr=0x%08X bComOwner=%d file=%s", (unsigned)hrCom, bComOwner, szFilename); DebugLogW(dbg, LOG_VERBOSE); }

    // Load image via WIC
    IWICImagingFactory* pWIC = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&pWIC));
    if (FAILED(hr) || !pWIC) {
      { wchar_t dbg[256]; swprintf(dbg, 256, L"LoadTex: WIC factory FAILED hr=0x%08X", (unsigned)hr); DebugLogW(dbg, LOG_WARN); }
      if (bComOwner) CoUninitialize();
      return TEXMGR_ERR_BADFILE;
    }

    IWICBitmapDecoder* pDecoder = nullptr;
    hr = pWIC->CreateDecoderFromFilename(szFilename, nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnLoad, &pDecoder);
    if (FAILED(hr) || !pDecoder) {
      { wchar_t dbg[256]; swprintf(dbg, 256, L"LoadTex: CreateDecoder FAILED hr=0x%08X", (unsigned)hr); DebugLogW(dbg, LOG_WARN); }
      pWIC->Release(); if (bComOwner) CoUninitialize(); return TEXMGR_ERR_BADFILE;
    }

    IWICBitmapFrameDecode* pFrame = nullptr;
    hr = pDecoder->GetFrame(0, &pFrame);
    if (FAILED(hr) || !pFrame) {
      { wchar_t dbg[256]; swprintf(dbg, 256, L"LoadTex: GetFrame FAILED hr=0x%08X", (unsigned)hr); DebugLogW(dbg, LOG_WARN); }
      pDecoder->Release(); pWIC->Release(); if (bComOwner) CoUninitialize(); return TEXMGR_ERR_BADFILE;
    }

    UINT imgW = 0, imgH = 0;
    pFrame->GetSize(&imgW, &imgH);

    // Convert to 32-bit BGRA
    IWICFormatConverter* pConverter = nullptr;
    pWIC->CreateFormatConverter(&pConverter);
    if (!pConverter) { pFrame->Release(); pDecoder->Release(); pWIC->Release(); if (bComOwner) CoUninitialize(); return TEXMGR_ERR_BADFILE; }

    pConverter->Initialize(pFrame, GUID_WICPixelFormat32bppBGRA,
                           WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeCustom);

    UINT srcRowPitch = imgW * 4;
    UINT totalBytes = srcRowPitch * imgH;
    std::vector<BYTE> pixels(totalBytes);
    pConverter->CopyPixels(nullptr, srcRowPitch, totalBytes, pixels.data());

    pConverter->Release();
    pFrame->Release();
    pDecoder->Release();
    pWIC->Release();
    if (bComOwner) CoUninitialize();

    // Create DX12 texture
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = imgW;
    texDesc.Height           = imgH;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    hr = dev->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&m_tex[iSlot].dx12Surface.resource));
    if (FAILED(hr)) return TEXMGR_ERR_OUTOFMEM;

    m_tex[iSlot].dx12Surface.width  = imgW;
    m_tex[iSlot].dx12Surface.height = imgH;
    m_tex[iSlot].dx12Surface.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_tex[iSlot].dx12Surface.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Create upload buffer with aligned row pitch
    UINT rowPitch = (imgW * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                    & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    UINT64 uploadSize = (UINT64)rowPitch * imgH;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width            = uploadSize;
    bufDesc.Height           = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels        = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = dev->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&m_tex[iSlot].dx12UploadBuf));
    if (FAILED(hr)) { m_tex[iSlot].dx12Surface.Reset(); return TEXMGR_ERR_OUTOFMEM; }

    // Copy pixels to upload buffer (with row pitch alignment)
    BYTE* mapped = nullptr;
    m_tex[iSlot].dx12UploadBuf->Map(0, nullptr, (void**)&mapped);
    for (UINT row = 0; row < imgH; row++)
      memcpy(mapped + row * rowPitch, pixels.data() + row * srcRowPitch, srcRowPitch);
    m_tex[iSlot].dx12UploadBuf->Unmap(0, nullptr);

    // Issue GPU copy
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_tex[iSlot].dx12UploadBuf.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width    = imgW;
    src.PlacedFootprint.Footprint.Height   = imgH;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_tex[iSlot].dx12Surface.resource.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition to shader resource
    m_lpDX12->TransitionResource(m_tex[iSlot].dx12Surface, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Write SRV at pre-allocated descriptor slot (no bump allocation)
    {
      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
      srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc.Format            = DXGI_FORMAT_B8G8R8A8_UNORM;
      srvDesc.ViewDimension     = D3D12_SRV_DIMENSION_TEXTURE2D;
      srvDesc.Texture2D.MipLevels = 1;
      dev->CreateShaderResourceView(m_tex[iSlot].dx12Surface.resource.Get(), &srvDesc,
          m_lpDX12->GetSrvCpuHandleAt(m_tex[iSlot].dx12Surface.srvIndex));

      // Update binding block in-place (slot 0 = texture, slots 1-15 = null)
      m_lpDX12->UpdateBindingBlockTexture(
          m_tex[iSlot].dx12Surface.bindingBlockStart,
          m_tex[iSlot].dx12Surface.srvIndex);
    }

    // Set sentinel so existing pSurface checks still work
    m_tex[iSlot].pSurface = (LPDIRECT3DTEXTURE9)(intptr_t)1;
    m_tex[iSlot].img_w = imgW;
    m_tex[iSlot].img_h = imgH;
  }

  m_tex[iSlot].fStartTime = time;
  m_tex[iSlot].nStartFrame = frame;

  int ret = TEXMGR_ERR_SUCCESS;

  // compile & run init. code:	
  if (!RunInitCode(iSlot, szInitCode))
    ret |= TEXMGR_WARN_ERROR_IN_INIT_CODE;

  // compile & save per-frame code:
  strcpy(m_tex[iSlot].m_szExpr, szCode);
  FreeCode(iSlot);
  if (!RecompileExpressions(iSlot))
    ret |= TEXMGR_WARN_ERROR_IN_REG_CODE;

  //g_dumpmsg("texmgr: success");

  return ret;
}

void texmgr::KillTex(int iSlot) {
  if (iSlot < 0) return;
  if (iSlot >= NUM_TEX) return;

  // Release GPU resource but preserve pre-allocated SRV indices
  m_tex[iSlot].dx12Surface.ResetResource();
  m_tex[iSlot].dx12UploadBuf.Reset();

  // Clear the pre-allocated SRV descriptor and binding block to null
  if (m_lpDX12 && m_tex[iSlot].dx12Surface.srvIndex != UINT_MAX) {
    m_lpDX12->m_device->CopyDescriptorsSimple(1,
        m_lpDX12->GetSrvCpuHandleAt(m_tex[iSlot].dx12Surface.srvIndex),
        m_lpDX12->GetSrvCpuHandleAt(m_lpDX12->m_nullTexture.srvIndex),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_lpDX12->UpdateBindingBlockTexture(
        m_tex[iSlot].dx12Surface.bindingBlockStart, UINT_MAX);
  }

  // Free old DX9 resources:
  if (m_tex[iSlot].pSurface && m_tex[iSlot].pSurface != (LPDIRECT3DTEXTURE9)(intptr_t)1) {
    int refcount = 0;
    for (int x = 0; x < NUM_TEX; x++)
      if (m_tex[x].pSurface == m_tex[iSlot].pSurface)
        refcount++;

    if (refcount == 1)
      m_tex[iSlot].pSurface->Release();
  }
  m_tex[iSlot].pSurface = NULL;
  m_tex[iSlot].szFileName[0] = 0;

  FreeCode(iSlot);
}

void texmgr::StripLinefeedCharsAndComments(char* src, char* dest) {
  // replaces all LINEFEED_CONTROL_CHAR characters in src with a space in dest;
  // also strips out all comments (beginning with '//' and going til end of line).
  // Restriction: sizeof(dest) must be >= sizeof(src).

  int i2 = 0;
  int len = (int)strlen(src);
  int bComment = false;
  for (int i = 0; i < len; i++) {
    if (bComment) {
      if (src[i] == LINEFEED_CONTROL_CHAR)
        bComment = false;
    }
    else {
      if ((src[i] == '\\' && src[i + 1] == '\\') || (src[i] == '/' && src[i + 1] == '/'))
        bComment = true;
      else if (src[i] != LINEFEED_CONTROL_CHAR)
        dest[i2++] = src[i];
    }
  }
  dest[i2] = 0;
}

bool texmgr::RunInitCode(int iSlot, char* szInitCode) {
  // warning: destroys contents of m_tex[iSlot].m_szExpr,
  //   so be sure to call RunInitCode before writing or
  //   compiling that string!

  FreeCode(iSlot);
  FreeVars(iSlot);
  RegisterBuiltInVariables(iSlot);

  strcpy(m_tex[iSlot].m_szExpr, szInitCode);
  bool ret = RecompileExpressions(iSlot);

  // set default values of output variables:
  // (by not setting these every frame, we allow the values to persist from frame-to-frame.)
  *(m_tex[iSlot].var_x) = 0.5;
  *(m_tex[iSlot].var_y) = 0.5;
  *(m_tex[iSlot].var_sx) = 1.0;
  *(m_tex[iSlot].var_sy) = 1.0;
  *(m_tex[iSlot].var_repeatx) = 1.0;
  *(m_tex[iSlot].var_repeaty) = 1.0;
  *(m_tex[iSlot].var_rot) = 0.0;
  *(m_tex[iSlot].var_flipx) = 0.0;
  *(m_tex[iSlot].var_flipy) = 0.0;
  *(m_tex[iSlot].var_r) = 1.0;
  *(m_tex[iSlot].var_g) = 1.0;
  *(m_tex[iSlot].var_b) = 1.0;
  *(m_tex[iSlot].var_a) = 1.0;
  *(m_tex[iSlot].var_blendmode) = 0.0;
  *(m_tex[iSlot].var_done) = 0.0;
  *(m_tex[iSlot].var_burn) = 1.0;

#ifndef _NO_EXPR_
  if (m_tex[iSlot].m_codehandle)
    NSEEL_code_execute(m_tex[iSlot].m_codehandle);
#endif

  return ret;
}

bool texmgr::RecompileExpressions(int iSlot) {
  char* expr = m_tex[iSlot].m_szExpr;

  // QUICK FIX: if the string ONLY has spaces and linefeeds, erase it, 
  // because for some strange reason this would cause an error in compileCode().
  {
    char* p = expr;
    while (*p == ' ' || *p == LINEFEED_CONTROL_CHAR) p++;
    if (*p == 0) expr[0] = 0;
  }

  // replace linefeed control characters with spaces, so they don't mess up the code compiler,
  // and strip out any comments ('//') before sending to CompileCode().
  char buf[sizeof(m_tex[iSlot].m_szExpr)];
  StripLinefeedCharsAndComments(expr, buf);

  // LJ DEBUG
  // ====================================
  // This was missing in BeatDrop
  if (buf[0]) {
#ifndef _NO_EXPR_
    //resetVars(m_tex[iSlot].m_vars);
    //g_dumpmsg("texmgr: compiling string: ");
    //g_dumpmsg(buf);
    if (!(m_tex[iSlot].m_codehandle = NSEEL_code_compile(m_tex[iSlot].tex_eel_ctx, buf, 0))) {
      //g_dumpmsg(" -error!");
      //MessageBox( NULL, "error in per-frame code", "MILKDROP ERROR", MB_OK|MB_SETFOREGROUND|MB_TOPMOST );
      //sprintf(pg->m_szUserMessage, "warning: preset \"%s\": error in 'per_frame' code", m_szDesc);
      //pg->m_fShowUserMessageUntilThisTime = pg->m_fAnimTime + 6.0f;
    }
    else {
      //g_dumpmsg(" -ok!");
      //pg->m_fShowUserMessageUntilThisTime = pg->m_fAnimTime;	// clear any old error msg.
    }
    //resetVars(NULL);

    return (m_tex[iSlot].m_codehandle != 0);

#endif
  }
  // ====================================


  return true;
}

void texmgr::FreeVars(int iSlot) {
  // free the built-in variables AND any user variables
}

void texmgr::FreeCode(int iSlot) {
  // free the compiled expressions
  if (m_tex[iSlot].m_codehandle) {
    NSEEL_code_free(m_tex[iSlot].m_codehandle);
    m_tex[iSlot].m_codehandle = NULL;
  }
}

void texmgr::RegisterBuiltInVariables(int iSlot) {
  NSEEL_VMCTX eel_ctx = m_tex[iSlot].tex_eel_ctx;
  NSEEL_VM_resetvars(eel_ctx);

  // input variables
  m_tex[iSlot].var_time = NSEEL_VM_regvar(eel_ctx, "time");
  m_tex[iSlot].var_frame = NSEEL_VM_regvar(eel_ctx, "frame");
  m_tex[iSlot].var_fps = NSEEL_VM_regvar(eel_ctx, "fps");
  m_tex[iSlot].var_progress = NSEEL_VM_regvar(eel_ctx, "progress");
  m_tex[iSlot].var_bass = NSEEL_VM_regvar(eel_ctx, "bass");
  m_tex[iSlot].var_bass_att = NSEEL_VM_regvar(eel_ctx, "bass_att");
  m_tex[iSlot].var_mid = NSEEL_VM_regvar(eel_ctx, "mid");
  m_tex[iSlot].var_mid_att = NSEEL_VM_regvar(eel_ctx, "mid_att");
  m_tex[iSlot].var_treb = NSEEL_VM_regvar(eel_ctx, "treb");
  m_tex[iSlot].var_treb_att = NSEEL_VM_regvar(eel_ctx, "treb_att");

  // output variables
  m_tex[iSlot].var_x = NSEEL_VM_regvar(eel_ctx, "x");
  m_tex[iSlot].var_y = NSEEL_VM_regvar(eel_ctx, "y");
  m_tex[iSlot].var_sx = NSEEL_VM_regvar(eel_ctx, "sx");
  m_tex[iSlot].var_sy = NSEEL_VM_regvar(eel_ctx, "sy");
  m_tex[iSlot].var_repeatx = NSEEL_VM_regvar(eel_ctx, "repeatx");
  m_tex[iSlot].var_repeaty = NSEEL_VM_regvar(eel_ctx, "repeaty");
  m_tex[iSlot].var_rot = NSEEL_VM_regvar(eel_ctx, "rot");
  m_tex[iSlot].var_flipx = NSEEL_VM_regvar(eel_ctx, "flipx");
  m_tex[iSlot].var_flipy = NSEEL_VM_regvar(eel_ctx, "flipy");
  m_tex[iSlot].var_r = NSEEL_VM_regvar(eel_ctx, "r");
  m_tex[iSlot].var_g = NSEEL_VM_regvar(eel_ctx, "g");
  m_tex[iSlot].var_b = NSEEL_VM_regvar(eel_ctx, "b");
  m_tex[iSlot].var_a = NSEEL_VM_regvar(eel_ctx, "a");
  m_tex[iSlot].var_blendmode = NSEEL_VM_regvar(eel_ctx, "blendmode");
  m_tex[iSlot].var_done = NSEEL_VM_regvar(eel_ctx, "done");
  m_tex[iSlot].var_burn = NSEEL_VM_regvar(eel_ctx, "burn");
  m_tex[iSlot].var_layer = NSEEL_VM_regvar(eel_ctx, "layer");

  //	resetVars(NULL);
}