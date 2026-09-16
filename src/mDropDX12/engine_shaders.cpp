/*
  Plugin module: Shader Compilation & Caching
  Extracted from engine.cpp for maintainability.
  Contains: VShaderInfo, PShaderInfo, CShaderParams, RecompileVShader, RecompilePShader,
            LoadShaders, CreateDX12PresetPSOs, LoadShaderFromMemory, GenWarpPShaderText,
            GenCompPShaderText, SaveShaderBytecodeToFile, LoadShaderBytecodeFromFile, crc32
*/

#include "engine.h"
#include <direct.h>  // _mkdir
#include "shader_overrides.h"
#include "engine_helpers.h"
#include "format_to.h"
#include "utility.h"
#include "support.h"
#include "resource.h"
#include "defines.h"
#include "shell_defines.h"
#include "wasabi.h"
#include <assert.h>
#include <strsafe.h>

#include <Windows.h>
#include <cstdint>
#include <fstream>
namespace mdrop {

extern Engine g_engine;

void VShaderInfo::Clear() {
  SafeRelease(ptr);
  SafeRelease(CT);
  params.Clear();
}
void PShaderInfo::Clear() {
  SafeRelease(ptr);
  SafeRelease(CT);
  SafeRelease(bytecodeBlob);
  params.Clear();
}

void RotatePShaderSet(PShaderSet& dst, PShaderSet& src) {
  PShaderInfo* const d[] = { &dst.warp, &dst.comp, &dst.bufferA, &dst.bufferB, &dst.bufferC, &dst.bufferD };
  PShaderInfo* const s[] = { &src.warp, &src.comp, &src.bufferA, &src.bufferB, &src.bufferC, &src.bufferD };
  for (int i = 0; i < 6; i++) {
    // One move. This used to be Clear / shallow-copy / null out three pointers
    // / Clear the source's params -- the transfer written by hand, which is
    // what PShaderInfo's move assignment now is. It releases what dst held,
    // takes src's three COM references and its params, and leaves src holding
    // nothing so its destructor has nothing to release (#5).
    *d[i] = std::move(*s[i]);
  }
}

// global_CShaderParams_master_list and OnTextureEvict are gone (issue 17).
//
// The registry could never hold anything. Its only writer was the constructor:
//
//     if (global_CShaderParams_master_list.size() > 0)
//       global_CShaderParams_master_list.push_back(this);
//
// which appends only when the list is ALREADY non-empty, and nothing else ever
// pushed. So it started empty and stayed empty for the life of the process, the
// destructor's erase loop iterated nothing, and both eviction walks -- at
// engine.cpp and engine_textures.cpp -- ran over zero elements every time.
//
// It was intended to NULL out any texptr a CShaderParams still held when a
// texture was evicted. What actually prevents that dangling bind is the age
// filter in EvictSomeTexture: `nAge < m_nPresetsLoadedTotal - 1` excludes every
// texture the CURRENT preset uses, and the -1 keeps the blend-from preset's
// too. That filter is real and is doing the work; this never did any.
CShaderParams::CShaderParams() {}

CShaderParams::~CShaderParams() {
  texsize_params.clear();
}

void CShaderParams::Clear() {
  // float4 handles:
  rand_frame = NULL;
  rand_preset = NULL;
  fft_params = NULL;

  ZeroMemory(rot_mat, sizeof(rot_mat));
  ZeroMemory(const_handles, sizeof(const_handles));
  ZeroMemory(q_const_handles, sizeof(q_const_handles));
  texsize_params.clear();

  // sampler stages for various PS texture bindings:
  for (int i = 0; i < sizeof(m_texture_bindings) / sizeof(m_texture_bindings[0]); i++) {
    m_texture_bindings[i].texptr = NULL;
    m_texture_bindings[i].dx12SrvIndex = UINT_MAX;
    m_texcode[i] = TEX_DISK;
  }
}

void CShaderParams::CacheParams(LPD3DXCONSTANTTABLE pCT, bool bHardErrors) {
  Clear();

  if (!pCT)
    return;

  D3DXCONSTANTTABLE_DESC d;
  pCT->GetDesc(&d);

  D3DXCONSTANT_DESC cd;

#define MAX_RAND_TEX 16
  std::wstring RandTexName[MAX_RAND_TEX];

  // Diagnostic file for Shadertoy sampler debugging (Verbose only)
  FILE* fpDiag = nullptr;
  if (DLOG_DIAG_ENABLED() && (g_engine.m_bLoadingShadertoyMode || g_engine.m_bShadertoyMode)) {
    fpDiag = DebugLogDiagOpen(L"diag_cacheparams.txt", L"a");
    if (fpDiag) fprintf(fpDiag, "=== CacheParams: %u constants ===\n", d.Constants);
  }

  DLOG_VERBOSE("DX12: CacheParams: %u constants", d.Constants);

  // pass 1: find all the samplers (and texture bindings).
  for (UINT i = 0; i < d.Constants; i++) {
    D3DXHANDLE h = pCT->GetConstant(NULL, i);
    unsigned int count = 1;
    pCT->GetConstantDesc(h, &cd, &count);

    if (fpDiag) fprintf(fpDiag, "  [%u] Name=%s RegSet=%d RegIdx=%d Type=%d\n", i, cd.Name ? cd.Name : "(null)", cd.RegisterSet, cd.RegisterIndex, cd.Type);
    DLOG_VERBOSE("DX12: CacheParams pass1: [%u] Name=%s RegSet=%d RegIdx=%d", i, cd.Name ? cd.Name : "(null)", cd.RegisterSet, cd.RegisterIndex);

    // cd.Name          = VS_Sampler
    // cd.RegisterSet   = D3DXRS_SAMPLER
    // cd.RegisterIndex = 3
    if (cd.RegisterSet == D3DXRS_SAMPLER && cd.RegisterIndex >= 0 && cd.RegisterIndex < sizeof(m_texture_bindings) / sizeof(m_texture_bindings[0])) {
      assert(m_texture_bindings[cd.RegisterIndex].texptr == NULL);

      // remove "sampler_" prefix to create root file name.  could still have "FW_" prefix or something like that.
      wchar_t szRootName[MAX_PATH];
      if (!strncmp(cd.Name, "sampler_", 8))
        CopyTo(szRootName, AutoWide(&cd.Name[8]));
      else
        CopyTo(szRootName, AutoWide(cd.Name));

      // also peel off "XY_" prefix, if it's there, to specify filtering & wrap mode.
      bool bBilinear = true;
      bool bWrap = true;
      bool bWrapFilterSpecified = false;
      if (lstrlenW(szRootName) > 3 && szRootName[2] == L'_') {
        wchar_t temp[3];
        temp[0] = szRootName[0];
        temp[1] = szRootName[1];
        temp[2] = 0;
        // convert to uppercase
        if (temp[0] >= L'a' && temp[0] <= L'z')
          temp[0] -= L'a' - L'A';
        if (temp[1] >= L'a' && temp[1] <= L'z')
          temp[1] -= L'a' - L'A';

        if (!wcscmp(temp, L"FW")) { bWrapFilterSpecified = true; bBilinear = true;  bWrap = true; }
        else if (!wcscmp(temp, L"FC")) { bWrapFilterSpecified = true; bBilinear = true;  bWrap = false; }
        else if (!wcscmp(temp, L"PW")) { bWrapFilterSpecified = true; bBilinear = false; bWrap = true; }
        else if (!wcscmp(temp, L"PC")) { bWrapFilterSpecified = true; bBilinear = false; bWrap = false; }
        // also allow reverses:
        else if (!wcscmp(temp, L"WF")) { bWrapFilterSpecified = true; bBilinear = true;  bWrap = true; }
        else if (!wcscmp(temp, L"CF")) { bWrapFilterSpecified = true; bBilinear = true;  bWrap = false; }
        else if (!wcscmp(temp, L"WP")) { bWrapFilterSpecified = true; bBilinear = false; bWrap = true; }
        else if (!wcscmp(temp, L"CP")) { bWrapFilterSpecified = true; bBilinear = false; bWrap = false; }

        // peel off the prefix
        int dst = 0;
        while (szRootName[dst + 3]) {
          szRootName[dst] = szRootName[dst + 3];
          dst++;
        }
        szRootName[dst] = 0;
      }
      m_texture_bindings[cd.RegisterIndex].bWrap = bWrap;
      m_texture_bindings[cd.RegisterIndex].bBilinear = bBilinear;

      // if <szFileName> is "main", map it to the VS...
      if (!wcscmp(L"main", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_VS;
      }
      else if (!wcscmp(L"feedback", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_FEEDBACK;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;   // default CLAMP for feedback
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
      else if (!wcscmp(L"image", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_IMAGE_FEEDBACK;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;   // default CLAMP
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
      else if (!wcscmp(L"audio", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_AUDIO;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;   // default CLAMP
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
      else if (!wcscmp(L"bufferB", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_BUFFER_B;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;   // default CLAMP for feedback
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
      else if (!wcscmp(L"bufferC", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_BUFFER_C;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
      else if (!wcscmp(L"bufferD", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = NULL;
        m_texcode[cd.RegisterIndex] = TEX_BUFFER_D;
        if (!bWrapFilterSpecified) {
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#if (NUM_BLUR_TEX >= 2)
      else if (!wcscmp(L"blur1", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[1];
        m_texcode[cd.RegisterIndex] = TEX_BLUR1;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
#if (NUM_BLUR_TEX >= 4)
      else if (!wcscmp(L"blur2", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[3];
        m_texcode[cd.RegisterIndex] = TEX_BLUR2;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
#if (NUM_BLUR_TEX >= 6)
      else if (!wcscmp(L"blur3", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[5];
        m_texcode[cd.RegisterIndex] = TEX_BLUR3;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
#if (NUM_BLUR_TEX >= 8)
      else if (!wcscmp("blur4", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[7];
        m_texcode[cd.RegisterIndex] = TEX_BLUR4;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
#if (NUM_BLUR_TEX >= 10)
      else if (!wcscmp("blur5", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[9];
        m_texcode[cd.RegisterIndex] = TEX_BLUR5;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
#if (NUM_BLUR_TEX >= 12)
      else if (!wcscmp("blur6", szRootName)) {
        m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_lpBlur[11];
        m_texcode[cd.RegisterIndex] = TEX_BLUR6;
        if (!bWrapFilterSpecified) { // when sampling blur textures, default is CLAMP
          m_texture_bindings[cd.RegisterIndex].bWrap = false;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
        }
      }
#endif
      else if (!wcsncmp(L"chtex", szRootName, 5) &&
               szRootName[5] >= L'0' && szRootName[5] <= L'3' && szRootName[6] == 0) {
        // Custom channel texture from Shader Import UI (sampler_chtex0..3)
        int chIdx = szRootName[5] - L'0';
        m_texcode[cd.RegisterIndex] = TEX_DISK;
        if (g_engine.m_dx12ChannelTex[chIdx].IsValid()) {
          m_texture_bindings[cd.RegisterIndex].dx12SrvIndex = g_engine.m_dx12ChannelTex[chIdx].srvIndex;
          m_texture_bindings[cd.RegisterIndex].bWrap = true;
          m_texture_bindings[cd.RegisterIndex].bBilinear = true;
          DLOG_INFO("CacheParams: sampler_chtex%d → loaded texture (srv=%u)",
            chIdx, g_engine.m_dx12ChannelTex[chIdx].srvIndex);
        } else if (!g_engine.m_szChannelTexPath[chIdx].empty()) {
          // Load the texture file now
          DX12Texture tex = g_engine.m_lpDX->LoadTextureFromFile(g_engine.m_szChannelTexPath[chIdx].c_str());
          if (tex.IsValid()) {
            g_engine.m_dx12ChannelTex[chIdx] = tex;
            m_texture_bindings[cd.RegisterIndex].dx12SrvIndex = tex.srvIndex;
            m_texture_bindings[cd.RegisterIndex].bWrap = true;
            m_texture_bindings[cd.RegisterIndex].bBilinear = true;
            DLOG_INFO("CacheParams: sampler_chtex%d → loaded '%ls' (srv=%u)",
              chIdx, g_engine.m_szChannelTexPath[chIdx].c_str(), tex.srvIndex);
          } else {
            DLOG_WARN("CacheParams: sampler_chtex%d → FAILED to load '%ls'",
              chIdx, g_engine.m_szChannelTexPath[chIdx].c_str());
          }
        }
      }
      else {
        m_texcode[cd.RegisterIndex] = TEX_DISK;

        // check for request for random texture.
        if (!wcsncmp(L"rand", szRootName, 4) &&
          IsNumericChar(szRootName[4]) &&
          IsNumericChar(szRootName[5]) &&
          (szRootName[6] == 0 || szRootName[6] == '_')) {
          int rand_slot = -1;

          // peel off filename prefix ("rand13_smalltiled", for example)
          wchar_t prefix[MAX_PATH];
          if (szRootName[6] == L'_')
            CopyTo(prefix, &szRootName[7]);
          else
            prefix[0] = 0;
          szRootName[6] = 0;

          swscanf(&szRootName[4], L"%d", &rand_slot);
          if (rand_slot >= 0 && rand_slot <= 15)      // otherwise, not a special filename - ignore it
          {
            if (!PickRandomTexture(prefix, szRootName)) {
              if (prefix[0])
                FormatTo(szRootName, L"[rand%02d] %s*", rand_slot, prefix);
              else
                FormatTo(szRootName, L"[rand%02d] *", rand_slot);
            }
            else {
              //chop off extension
              wchar_t* p = wcsrchr(szRootName, L'.');
              if (p)
                *p = 0;
            }

            RandTexName[rand_slot] = szRootName; // we'll need to remember this for texsize_ params!
          }
        }

        // see if <szRootName>.tga or .jpg has already been loaded.
        //   (if so, grab a pointer to it)
        //   (if NOT, create & load it).
        int N = (int)g_engine.m_textures.size();
        for (int n = 0; n < N; n++) {
          if (!wcscmp(g_engine.m_textures[n].texname, szRootName)) {
            // found a match - texture was already loaded
            m_texture_bindings[cd.RegisterIndex].texptr = g_engine.m_textures[n].texptr;
            m_texture_bindings[cd.RegisterIndex].dx12SrvIndex = g_engine.m_textures[n].dx12Tex.srvIndex;
            // also bump its age down to zero! (for cache mgmt)
            g_engine.m_textures[n].nAge = g_engine.m_nPresetsLoadedTotal;
            break;
          }
        }
        // if still not found, load it up / make a new texture
        if (!m_texture_bindings[cd.RegisterIndex].texptr &&
            m_texture_bindings[cd.RegisterIndex].dx12SrvIndex == UINT_MAX) {
          TexInfo x;
          CopyTo(x.texname, szRootName);
          x.texptr = NULL;

          // Built-in procedurally generated textures never exist on disk.
          // If missing from m_textures[] the device is mid-reinit; skip silently.
          // They will be regenerated by AllocateMyDX9Stuff() and the preset
          // will be re-cached at that point.
          {
            static const wchar_t* kBuiltinNoise[] = {
                L"noise_lq", L"noise_lq_lite", L"noise_mq", L"noise_hq",
                L"noisevol_lq", L"noisevol_hq",
                L"noise_lq_st", L"noise_mq_st", L"noise_hq_st",
                L"noisevol_lq_st", L"noisevol_hq_st", nullptr
            };
            bool bBuiltin = false;
            for (int k = 0; kBuiltinNoise[k]; k++)
                if (!wcscmp(szRootName, kBuiltinNoise[k])) { bBuiltin = true; break; }
            if (bBuiltin)
                continue;
          }

          {
            // Load the texture through WIC. This was the !GetDevice() arm of a
            // DX9/DX12 pair (issue 17); the DX9 arm below it is gone, so the
            // condition is gone with it and the block is kept only for scope.
            // DX12 path: load via WIC
            wchar_t szFilename[MAX_PATH];
            bool found = false;
            DLOG_VERBOSE("CacheParams: searching for texture '%ls'", szRootName);
            for (int z = 0; z < texture_exts_count; z++) {
              FormatTo(szFilename, L"%stextures\\%s.%s", g_engine.m_szMilkdrop2Path, szRootName, texture_exts[z].c_str());
              if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                FormatTo(szFilename, L"%s%s.%s", g_engine.m_szPresetDir, szRootName, texture_exts[z].c_str());
                if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                  // Check for textures\ sibling of preset directory
                  bool siblingFound = false;
                  {
                    wchar_t siblingTexDir[MAX_PATH];
                    CopyTo(siblingTexDir, g_engine.m_szPresetDir);
                    int len = (int)wcslen(siblingTexDir);
                    if (len > 0 && siblingTexDir[len - 1] == L'\\') siblingTexDir[--len] = 0;
                    wchar_t* lastSlash = wcsrchr(siblingTexDir, L'\\');
                    if (lastSlash) {
                      lastSlash[1] = 0;
                      AppendTo(siblingTexDir, L"textures\\");
                      FormatTo(szFilename, L"%s%s.%s", siblingTexDir, szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) siblingFound = true;
                    }
                  }
                  if (!siblingFound) {
                    // Search dedicated random textures directory
                    bool fbFound = false;
                    if (!fbFound && g_engine.m_szRandomTexDir[0]) {
                      FormatTo(szFilename, L"%s%s.%s", g_engine.m_szRandomTexDir, szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) fbFound = true;
                    }
                    // Search content base path
                    if (!fbFound && g_engine.m_szContentBasePath[0]) {
                      FormatTo(szFilename, L"%s%s.%s", g_engine.m_szContentBasePath, szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) fbFound = true;
                      if (!fbFound) {
                        FormatTo(szFilename, L"%stextures\\%s.%s", g_engine.m_szContentBasePath, szRootName, texture_exts[z].c_str());
                        if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) fbFound = true;
                      }
                    }
                    // Then search fallback paths (paths already have trailing backslash)
                    if (!fbFound) {
                      // A copy: this loop calls GetFileAttributesW per entry.
                      for (auto& fbPath : g_engine.FallbackPathsCopy()) {
                        FormatTo(szFilename, L"%s%s.%s", fbPath.c_str(), szRootName, texture_exts[z].c_str());
                        if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) { fbFound = true; break; }
                      }
                    }
                    // LAST: "../<whatever subdirectory>", the convention every
                    // pre-MDropDX12 build honoured (#299).
                    //
                    // The sibling check above is this same idea, but it only
                    // ever tried a folder named exactly "textures". MilkDrop 3
                    // PRO keeps them in "sprites", so a preset in
                    // Milkdrop3\presets1\ asking for 'spirale3' could not find
                    // Milkdrop3\sprites\Spirale3.jpg one folder over -- which
                    // is the report this came from.
                    //
                    // Last deliberately: every ordered path above keeps
                    // priority, so nothing that resolves today can start
                    // resolving somewhere else. This only ever converts a
                    // failure into a hit. The list is cached per preset
                    // directory, so the directory listing is not repeated for
                    // each texture and extension.
                    if (!fbFound) {
                      for (const auto& dir : g_engine.SiblingResourceDirs()) {
                        FormatTo(szFilename, L"%s%s.%s", dir.c_str(), szRootName, texture_exts[z].c_str());
                        if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) { fbFound = true; break; }
                      }
                    }
                    if (!fbFound) continue;
                  }
                }
              }
              x.dx12Tex = g_engine.m_lpDX->LoadTextureFromFile(szFilename);
              if (x.dx12Tex.resource) {
                x.w = x.dx12Tex.width;
                x.h = x.dx12Tex.height;
                x.d = 1;
                x.bEvictable = true;
                x.nAge = g_engine.m_nPresetsLoadedTotal;
                // What the device says it allocated, not w*h*4+16384 (#347).
                // That estimate was the BASE MIP ONLY, while this texture was
                // uploaded with a full mip chain -- 4/3 of the base, plus 64 KB
                // subresource alignment on the small levels. Measured live, the
                // unrecorded part was 39-40% of the reported cache size on every
                // preset load, and because it sat outside texMB it read as a
                // separate leak.
                x.nSizeInBytes = (int)g_engine.m_lpDX->TextureVramBytes(x.dx12Tex);
                found = true;
                DLOG_VERBOSE("CacheParams: loaded texture '%ls' from '%ls'", szRootName, szFilename);
                break;
              }
              // WIC couldn't decode this format (e.g. .dds) — try next extension
            }

            if (!found) {
              // Counted per preset (#307), so a sweep can say WHICH preset is
              // missing art. A missing texture is not a compile failure -- the
              // preset renders, just wrongly -- so nothing that asks "did the
              // shaders build" will ever notice it.
              g_engine.m_nTextureMissesThisLoad++;
              wchar_t buf[2048], title[64];
              FormatResTo(buf, IDS_COULD_NOT_LOAD_TEXTURE_X, szRootName, szExtsWithSlashes);
              g_engine.dumpmsg(buf, LOG_WARN);
              DLOG_VERBOSE("CacheParams: texture NOT found: '%ls' (base='%ls', preset='%ls', %d fallback paths)",
                        szRootName, g_engine.m_szMilkdrop2Path, g_engine.m_szPresetDir,
                        (int)g_engine.FallbackPathsCopy().size());
              if (bHardErrors)
                MessageBoxW(g_engine.GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
              else
                g_engine.AddError(buf, 6.0f, ERR_PRESET, true);
              continue;
            }

            g_engine.m_textures.push_back(x);
            // Keep the cache inside MaxBytes/MaxImages (#312). This is the only
            // place a preset's own images enter it, and nothing ever took any
            // of them back out.
            //
            // Safe here only because EnforceTextureCacheBudget drains the GPU
            // and takes the mirror engine lock before it touches anything --
            // calling the old eviction from this same spot produced a page
            // fault at VA 0 within a dozen presets. It returns immediately
            // when the cache is inside budget, so the drain is paid on the
            // loads that evict rather than on every load.
            g_engine.EnforceTextureCacheBudget();
            m_texture_bindings[cd.RegisterIndex].dx12SrvIndex = x.dx12Tex.srvIndex;
          }
        }
      }
    }
  }

  DebugLogA("DX12: CacheParams: pass 1 done, entering pass 2", LOG_VERBOSE);

  // pass 2: bind all the float4's.  "texsize_XYZ" params will be filled out via knowledge of loaded texture sizes.
  for (UINT i = 0; i < d.Constants; i++) {
    D3DXHANDLE h = pCT->GetConstant(NULL, i);
    unsigned int count = 1;
    pCT->GetConstantDesc(h, &cd, &count);

    DLOG_VERBOSE("DX12: CacheParams pass2: [%u] Name=%s RegSet=%d Class=%d", i, cd.Name ? cd.Name : "(null)", cd.RegisterSet, cd.Class);

    if (cd.RegisterSet == D3DXRS_FLOAT4) {
      if (cd.Class == D3DXPC_MATRIX_COLUMNS) {
        if (!strcmp(cd.Name, "rot_s1")) rot_mat[0] = h;
        else if (!strcmp(cd.Name, "rot_s2")) rot_mat[1] = h;
        else if (!strcmp(cd.Name, "rot_s3")) rot_mat[2] = h;
        else if (!strcmp(cd.Name, "rot_s4")) rot_mat[3] = h;
        else if (!strcmp(cd.Name, "rot_d1")) rot_mat[4] = h;
        else if (!strcmp(cd.Name, "rot_d2")) rot_mat[5] = h;
        else if (!strcmp(cd.Name, "rot_d3")) rot_mat[6] = h;
        else if (!strcmp(cd.Name, "rot_d4")) rot_mat[7] = h;
        else if (!strcmp(cd.Name, "rot_f1")) rot_mat[8] = h;
        else if (!strcmp(cd.Name, "rot_f2")) rot_mat[9] = h;
        else if (!strcmp(cd.Name, "rot_f3")) rot_mat[10] = h;
        else if (!strcmp(cd.Name, "rot_f4")) rot_mat[11] = h;
        else if (!strcmp(cd.Name, "rot_vf1")) rot_mat[12] = h;
        else if (!strcmp(cd.Name, "rot_vf2")) rot_mat[13] = h;
        else if (!strcmp(cd.Name, "rot_vf3")) rot_mat[14] = h;
        else if (!strcmp(cd.Name, "rot_vf4")) rot_mat[15] = h;
        else if (!strcmp(cd.Name, "rot_uf1")) rot_mat[16] = h;
        else if (!strcmp(cd.Name, "rot_uf2")) rot_mat[17] = h;
        else if (!strcmp(cd.Name, "rot_uf3")) rot_mat[18] = h;
        else if (!strcmp(cd.Name, "rot_uf4")) rot_mat[19] = h;
        else if (!strcmp(cd.Name, "rot_rand1")) rot_mat[20] = h;
        else if (!strcmp(cd.Name, "rot_rand2")) rot_mat[21] = h;
        else if (!strcmp(cd.Name, "rot_rand3")) rot_mat[22] = h;
        else if (!strcmp(cd.Name, "rot_rand4")) rot_mat[23] = h;
      }
      else if (cd.Class == D3DXPC_VECTOR) {
        if (!strcmp(cd.Name, "_fftParams")) fft_params = h;
        else if (!strcmp(cd.Name, "rand_frame"))  rand_frame = h;
        else if (!strcmp(cd.Name, "rand_preset")) rand_preset = h;
        else if (!strncmp(cd.Name, "texsize_", 8)) {
          // remove "texsize_" prefix to find root file name.
          wchar_t szRootName[MAX_PATH];
          if (!strncmp(cd.Name, "texsize_", 8))
            CopyTo(szRootName, AutoWide(&cd.Name[8]));
          else
            CopyTo(szRootName, AutoWide(cd.Name));

          // check for request for random texture.
          // it should be a previously-seen random index - just fetch/reuse the name.
          if (!wcsncmp(L"rand", szRootName, 4) &&
            IsNumericChar(szRootName[4]) &&
            IsNumericChar(szRootName[5]) &&
            (szRootName[6] == 0 || szRootName[6] == L'_')) {
            int rand_slot = -1;

            // ditch filename prefix ("rand13_smalltiled", for example)
            // and just go by the slot
            if (szRootName[6] == L'_')
              szRootName[6] = 0;

            swscanf(&szRootName[4], L"%d", &rand_slot);
            if (rand_slot >= 0 && rand_slot <= 15)      // otherwise, not a special filename - ignore it
              if (RandTexName[rand_slot].size() > 0)
                CopyTo(szRootName, RandTexName[rand_slot].c_str());
          }

          // see if <szRootName>.tga or .jpg has already been loaded.
          bool bTexFound = false;
          int N = (int)g_engine.m_textures.size();
          for (int n = 0; n < N; n++) {
            if (!wcscmp(g_engine.m_textures[n].texname, szRootName)) {
              // found a match - texture was loaded
              TexSizeParamInfo y;
              y.texname = szRootName; //for debugging
              y.texsize_param = h;
              y.w = g_engine.m_textures[n].w;
              y.h = g_engine.m_textures[n].h;
              texsize_params.push_back(y);

              bTexFound = true;
              break;
            }
          }

          // The texsize warning that stood here is gone (issue 17): its
          // condition ANDed in the DX9 device, which never existed under DX12,
          // so it could never fire. texsize_params is filled above this.
        }
        else if (cd.Name[0] == '_' && cd.Name[1] == 'c') {
          int z;
          if (sscanf(&cd.Name[2], "%d", &z) == 1)
            if (z >= 0 && z < sizeof(const_handles) / sizeof(const_handles[0]))
              const_handles[z] = h;
        }
        else if (cd.Name[0] == '_' && cd.Name[1] == 'q') {
          int z = cd.Name[2] - 'a';
          if (z >= 0 && z < sizeof(q_const_handles) / sizeof(q_const_handles[0]))
            q_const_handles[z] = h;
        }
      }
    }
  }

  if (fpDiag) {
    fprintf(fpDiag, "  Result texcodes:");
    for (int i = 0; i < 16; i++) {
      if (m_texcode[i] != 0 || m_texture_bindings[i].dx12SrvIndex != UINT_MAX)
        fprintf(fpDiag, " [%d]=%d(srv=%u)", i, m_texcode[i], m_texture_bindings[i].dx12SrvIndex);
    }
    fprintf(fpDiag, "\n---\n");
    fclose(fpDiag);
  }
  DebugLogA("DX12: CacheParams: pass 2 done, returning", LOG_VERBOSE);
}

//----------------------------------------------------------------------

thread_local const wchar_t* mdrop::Engine::t_shaderCompilePreset = nullptr;

bool Engine::RecompileVShader(const char* szShadersText, VShaderInfo* si, int shaderType, bool bHardErrors, bool bCompileOnly) {
  si->Clear();

  char ver[16];
  CopyToA(ver, "vs_1_1");

  // LOAD SHADER
  if (!LoadShaderFromMemory(szShadersText, "VS", ver, &si->CT, (void**)&si->ptr, shaderType, bHardErrors, bCompileOnly, nullptr))
    return false;

  if (!bCompileOnly) {
    // Track down texture & float4 param bindings for this shader.
    // Also loads any textures that need loaded.
    si->params.CacheParams(si->CT, bHardErrors);
  }

  return true;
}

bool Engine::RecompilePShader(const char* szShadersText, PShaderInfo* si, int shaderType, bool bHardErrors, int PSVersion, bool bCompileOnly, const char* szDiagName) {
  assert(m_nMaxPSVersion > 0);

  si->Clear();

  // LOAD SHADER
  // note: ps_1_4 required for dependent texture lookups.
  //       ps_2_0 required for tex2Dbias.
  char ver[16];
  CopyToA(ver, "ps_0_0");
  switch (PSVersion) {
  case MD2_PS_NONE:
    // Even though the PRESET doesn't use shaders, if MilkDrop is running where it CAN do shaders,
    //   we run all the old presets through (shader) emulation.
    // This way, during a MilkDrop session, we are always calling either WarpedBlit() or WarpedBlit_NoPixelShaders(),
    //   and blending always works.
    CopyToA(ver, "ps_2_0");
    break;
  case MD2_PS_2_0: CopyToA(ver, "ps_2_0"); break;
  case MD2_PS_2_X: CopyToA(ver, "ps_2_a"); break; // we'll try ps_2_a first, LoadShaderFromMemory will try ps_2_b if compilation fails
  case MD2_PS_3_0: CopyToA(ver, "ps_3_0"); break;
  case MD2_PS_4_0: CopyToA(ver, "ps_4_0"); break;
  case MD2_PS_5_0: CopyToA(ver, "ps_5_0"); break;
  default: assert(0); break;
  }

  if (!LoadShaderFromMemory(szShadersText, "PS", ver, &si->CT, (void**)&si->ptr, shaderType, bHardErrors, bCompileOnly, &si->bytecodeBlob, szDiagName)) {
    // Prefer loading/target desc over live m_pState (async milk3 compiles m_pNewState)
    const wchar_t* loadName = (m_szLoadingPreset[0] != 0) ? m_szLoadingPreset : m_szCurrentPresetFile;
    const wchar_t* newDesc = (m_pNewState && m_pNewState->m_szDesc[0]) ? m_pNewState->m_szDesc : L"";
    const wchar_t* liveDesc = (m_pState && m_pState->m_szDesc[0]) ? m_pState->m_szDesc : L"(unknown)";
    DebugLogWFmt(LOG_ERROR,
                 L"DX12: RecompilePShader FAILED type=%hs loading=%s newDesc=%s liveDesc=%s file=%s",
                 szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp"),
                 loadName, newDesc, liveDesc, m_szCurrentPresetFile);
    {
      char line[1024];
      FormatToA(line,
                "FAIL RecompilePShader type=%s loading=%ls newDesc=%ls liveDesc=%ls\n",
                szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp"),
                loadName, newDesc, liveDesc);
      DebugLogDiagAppend(L"diag_preset_load.txt", line);
    }
    return false;
  }

  DebugLogA("DX12: RecompilePShader: LoadShaderFromMemory OK, entering CacheParams...", LOG_VERBOSE);

  if (!bCompileOnly) {
    // Track down texture & float4 param bindings for this shader.
    // Also loads any textures that need loaded.
    si->params.CacheParams(si->CT, bHardErrors);
  }

  DebugLogA("DX12: RecompilePShader: CacheParams done, returning true", LOG_VERBOSE);
  return true;
}

bool Engine::LoadShaders(PShaderSet* sh, CState* pState, bool bTick, bool bCompileOnly) {
  // Truncate diagnostic file at start of each shader load (Verbose only)
  if (DLOG_DIAG_ENABLED() && m_bLoadingShadertoyMode && !bCompileOnly) {
    DebugLogDiagTruncate(L"diag_cacheparams.txt");
  }
  if (m_nMaxPSVersion <= 0) {
    DebugLogA("DX12: LoadShaders: m_nMaxPSVersion <= 0, skipping", LOG_VERBOSE);
    return true;
  }

  // load one of the pixel shaders
  DLOG_VERBOSE("DX12: LoadShaders: warp.ptr=%p warp.CT=%p nWarpPSVersion=%d nMaxPS=%d",
            (void*)sh->warp.ptr, (void*)sh->warp.CT, pState->m_nWarpPSVersion, m_nMaxPSVersion);
  if (!sh->warp.ptr && !sh->warp.CT && pState->m_nWarpPSVersion > 0) {
    // A tag rule may substitute the user's own warp shader for this preset's.
    // Only for the state actually being rendered or loaded -- CompilePresetShadersToFile
    // warms the cache for unrelated presets and must compile what is in them.
    const bool bOverrideOK = OverrideAppliesTo(pState) && !m_activeOverride.warpText.empty();
    const char* warpSrc = bOverrideOK ? m_activeOverride.warpText.c_str()
                                      : pState->m_szWarpShadersText;
    const int warpVer = bOverrideOK ? max(pState->m_nWarpPSVersion, MD2_PS_3_0)
                                    : pState->m_nWarpPSVersion;
    bool bOK = RecompilePShader(warpSrc, &sh->warp, SHADER_WARP, false, warpVer, bCompileOnly);
    if (!bOK && bOverrideOK) {
      // A broken override must not black-screen every preset a rule attaches it
      // to.  Fall back to the preset's own shader and blame the override.
      m_activeOverride.warpFailed = true;
      ShaderOverrides().SetLastError(m_activeOverride.name, L"warp shader failed to compile");
      DLOG_ERROR("ShaderOverride '%ls': warp failed to compile; using the preset's own",
                 m_activeOverride.name.c_str());
      bOK = RecompilePShader(pState->m_szWarpShadersText, &sh->warp, SHADER_WARP, false,
                             pState->m_nWarpPSVersion, bCompileOnly);
    }
    DLOG_VERBOSE("DX12: LoadShaders warp: bOK=%d bytecodeBlob=%p CT=%p ptr=%p",
              bOK, (void*)sh->warp.bytecodeBlob, (void*)sh->warp.CT, (void*)sh->warp.ptr);
    if (!bOK) {
      // switch to fallback shader
      // Assignment, not memcpy. PShaderInfo holds CShaderParams, which holds
      // std::vector<TexSizeParamInfo> texsize_params -- memcpy shallow-copied
      // that vector's internal pointers, so this object and the fallback ended
      // up owning the SAME heap buffer, and ~PShaderInfo() calls Clear() on
      // both. Whichever ran second freed memory the other still pointed at.
      // The AddRef calls above are unchanged: they cover the three COM
      // pointers, which the compiler-generated assignment copies raw.
      // CloneFrom, not assignment: this becomes a SECOND owner of the
      // fallback shader, which must stay valid for the next preset that
      // fails to compile. The three AddRefs that used to sit above this
      // line are inside CloneFrom now, so no site can cover two of them
      // and miss the third (#5).
      sh->warp.CloneFrom(m_fallbackShaders_ps.warp);
    }

    if (bTick)
      return true;
  }

  // Buffer A shader (Shadertoy two-pass) — each pass writes its own diag files directly
  // NOTE: Do NOT set m_bHasBufferA/B or m_bCompUsesFeedback here — this may run on a
  // background thread. Those flags are derived in LoadPresetTick after the shader swap.
  if (!sh->bufferA.ptr && !sh->bufferA.CT && pState->m_nBufferAPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szBufferAShadersText, &sh->bufferA, SHADER_COMP, false, pState->m_nBufferAPSVersion, bCompileOnly, "bufferA");
    DebugLogA(bOK ? "DX12: LoadShaders bufferA: compiled OK" : "DX12: LoadShaders bufferA: FAILED", bOK ? LOG_VERBOSE : LOG_ERROR);
  }

  // Buffer B shader (Shadertoy three-pass)
  if (!sh->bufferB.ptr && !sh->bufferB.CT && pState->m_nBufferBPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szBufferBShadersText, &sh->bufferB, SHADER_COMP, false, pState->m_nBufferBPSVersion, bCompileOnly, "bufferB");
    DebugLogA(bOK ? "DX12: LoadShaders bufferB: compiled OK" : "DX12: LoadShaders bufferB: FAILED", bOK ? LOG_VERBOSE : LOG_ERROR);
  }

  // Buffer C shader (Shadertoy four-pass)
  if (!sh->bufferC.ptr && !sh->bufferC.CT && pState->m_nBufferCPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szBufferCShadersText, &sh->bufferC, SHADER_COMP, false, pState->m_nBufferCPSVersion, bCompileOnly, "bufferC");
    DebugLogA(bOK ? "DX12: LoadShaders bufferC: compiled OK" : "DX12: LoadShaders bufferC: FAILED", bOK ? LOG_VERBOSE : LOG_ERROR);
  }

  // Buffer D shader (Shadertoy five-pass)
  if (!sh->bufferD.ptr && !sh->bufferD.CT && pState->m_nBufferDPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szBufferDShadersText, &sh->bufferD, SHADER_COMP, false, pState->m_nBufferDPSVersion, bCompileOnly, "bufferD");
    DebugLogA(bOK ? "DX12: LoadShaders bufferD: compiled OK" : "DX12: LoadShaders bufferD: FAILED", bOK ? LOG_VERBOSE : LOG_ERROR);
  }

  // Comp (Image) shader — compiled after bufferA/bufferB so diag_comp_shader.txt reflects comp
  if (!sh->comp.ptr && !sh->comp.CT && pState->m_nCompPSVersion > 0) {
    const bool bOverrideOK = OverrideAppliesTo(pState) && !m_activeOverride.compText.empty();
    const char* compSrc = bOverrideOK ? m_activeOverride.compText.c_str()
                                      : pState->m_szCompShadersText;
    const int compVer = bOverrideOK ? max(pState->m_nCompPSVersion, MD2_PS_3_0)
                                    : pState->m_nCompPSVersion;
    bool bOK = RecompilePShader(compSrc, &sh->comp, SHADER_COMP, false, compVer, bCompileOnly);
    if (!bOK && bOverrideOK) {
      m_activeOverride.compFailed = true;
      ShaderOverrides().SetLastError(m_activeOverride.name, L"comp shader failed to compile");
      DLOG_ERROR("ShaderOverride '%ls': comp failed to compile; using the preset's own",
                 m_activeOverride.name.c_str());
      bOK = RecompilePShader(pState->m_szCompShadersText, &sh->comp, SHADER_COMP, false,
                             pState->m_nCompPSVersion, bCompileOnly);
    }
    DLOG_VERBOSE("DX12: LoadShaders comp: bOK=%d bytecodeBlob=%p CT=%p ptr=%p",
              bOK, (void*)sh->comp.bytecodeBlob, (void*)sh->comp.CT, (void*)sh->comp.ptr);
    if (!bOK) {
      // switch to fallback shader
      // Assignment, not memcpy. PShaderInfo holds CShaderParams, which holds
      // std::vector<TexSizeParamInfo> texsize_params -- memcpy shallow-copied
      // that vector's internal pointers, so this object and the fallback ended
      // up owning the SAME heap buffer, and ~PShaderInfo() calls Clear() on
      // both. Whichever ran second freed memory the other still pointed at.
      // The AddRef calls above are unchanged: they cover the three COM
      // pointers, which the compiler-generated assignment copies raw.
      // CloneFrom, not assignment: this becomes a SECOND owner of the
      // fallback shader, which must stay valid for the next preset that
      // fails to compile. The three AddRefs that used to sit above this
      // line are inside CloneFrom now, so no site can cover two of them
      // and miss the third (#5).
      sh->comp.CloneFrom(m_fallbackShaders_ps.comp);
    }
  }

  return true;
}

void Engine::CreateDX12PresetPSOs(const PShaderSet& sh, const PShaderSet& oldSh,
                                  PresetPsoOwned& out) {
  if (!m_lpDX || !m_lpDX->m_device.Get() || !m_lpDX->m_rootSignature.Get())
    return;

  // No WaitForGpu here, deliberately (#184). The sync exists to protect the
  // RELEASE of the outgoing PSOs -- the GPU may still be executing a command
  // list that references them, and releasing those is the use-after-free / TDR
  // this function has always warned about. Building into a fresh `out` releases
  // nothing, so there is nothing to wait for.
  //
  // That distinction is what makes this callable off the render thread: a
  // render context compiling its own preset must NOT stall the whole device
  // from a worker, and engine.h records that worker-side operations on the main
  // queue have deadlocked D3D12Core and caused a TDR before. The sync now sits
  // with the assignment in the no-arg wrapper below, which is where the release
  // actually happens and which runs on the render thread.

  ID3D12Device* device = m_lpDX->m_device.Get();
  ID3D12RootSignature* rootSig = m_lpDX->m_rootSignature.Get();
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  // Create warp PSO from current shader bytecode
  out.warp.Reset();
  m_warpMainTexSlot = 0;
  if (sh.warp.bytecodeBlob && g_pWarpVSBlob) {
    out.warp = DX12CreatePresetPSO(
      device, rootSig, rtvFormat,
      g_pWarpVSBlob,
      sh.warp.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.warp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &m_warpMainTexSlot);
    if (m_warpMainTexSlot == UINT_MAX) m_warpMainTexSlot = 0;
  }

  // Create comp PSO from current shader bytecode
  // In Shadertoy mode (.milk3): comp/Image always writes to UNORM backbuffer.
  // In MilkDrop mode with single-pass feedback: comp writes to FLOAT32 feedback buffer.
  DXGI_FORMAT compRtvFormat;
  if (m_bShadertoyMode)
    compRtvFormat = rtvFormat;  // backbuffer UNORM
  else if (m_bCompUsesFeedback && !m_bHasBufferA)
    compRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
  else
    compRtvFormat = rtvFormat;
  out.comp.Reset();
  m_compMainTexSlot = 0;
  if (sh.comp.bytecodeBlob && g_pCompVSBlob) {
    out.comp = DX12CreatePresetPSO(
      device, rootSig, compRtvFormat,
      g_pCompVSBlob,
      sh.comp.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.comp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &m_compMainTexSlot);
    if (m_compMainTexSlot == UINT_MAX) m_compMainTexSlot = 0;
  }

  // Second copy of the same comp/Image shader for a FLOAT32 target. In .milk3
  // mode the Image pass renders into m_dx12ImageFeedback (FLOAT32) when the
  // preset samples its own previous Image (a `sampler_image` binding), then
  // blits that to the backbuffer — and the UNORM PSO above must not be bound to
  // a FLOAT32 render target. Built only for presets that actually take that
  // path, which is rare: none of the shipped .milk3 files do, so every other
  // preset would pay a PSO creation for nothing.
  bool compWantsImageFeedback = false;
  for (int i = 0; i < 16; i++) {
    if (sh.comp.params.m_texcode[i] == TEX_IMAGE_FEEDBACK)
      compWantsImageFeedback = true;
  }
  out.compFloat.Reset();
  if (m_bShadertoyMode && compWantsImageFeedback &&
      compRtvFormat != DXGI_FORMAT_R32G32B32A32_FLOAT &&
      sh.comp.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    out.compFloat = DX12CreatePresetPSO(
      device, rootSig, DXGI_FORMAT_R32G32B32A32_FLOAT,
      g_pCompVSBlob,
      sh.comp.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.comp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create Buffer A PSO — always renders to FLOAT32 feedback buffer (Shadertoy uses float32)
  DXGI_FORMAT feedbackRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
  out.bufferA.Reset();
  if (sh.bufferA.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    out.bufferA = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      sh.bufferA.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.bufferA.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create Buffer B PSO — same FLOAT32 feedback format as Buffer A
  out.bufferB.Reset();
  if (sh.bufferB.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    out.bufferB = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      sh.bufferB.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.bufferB.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create Buffer C PSO — same FLOAT32 feedback format
  out.bufferC.Reset();
  if (sh.bufferC.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    out.bufferC = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      sh.bufferC.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.bufferC.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create Buffer D PSO — same FLOAT32 feedback format
  out.bufferD.Reset();
  if (sh.bufferD.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    out.bufferD = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      sh.bufferD.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.bufferD.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create blend transition PSOs (old shader pass 0 + new shader alpha-blend pass 1)
  out.oldWarp.Reset();
  m_oldWarpMainTexSlot = 0;
  if (oldSh.warp.bytecodeBlob && g_pWarpVSBlob) {
    out.oldWarp = DX12CreatePresetPSO(
      device, rootSig, rtvFormat,
      g_pWarpVSBlob,
      oldSh.warp.bytecodeBlob->GetBufferPointer(),
      (UINT)oldSh.warp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &m_oldWarpMainTexSlot);
    if (m_oldWarpMainTexSlot == UINT_MAX) m_oldWarpMainTexSlot = 0;
  }

  out.warpBlend.Reset();
  if (sh.warp.bytecodeBlob && g_pWarpVSBlob) {
    out.warpBlend = DX12CreatePresetPSO(
      device, rootSig, rtvFormat,
      g_pWarpVSBlob,
      sh.warp.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.warp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      true, nullptr);  // alphaBlend=true
  }

  out.oldComp.Reset();
  m_oldCompMainTexSlot = 0;
  if (oldSh.comp.bytecodeBlob && g_pCompVSBlob) {
    out.oldComp = DX12CreatePresetPSO(
      device, rootSig, compRtvFormat,
      g_pCompVSBlob,
      oldSh.comp.bytecodeBlob->GetBufferPointer(),
      (UINT)oldSh.comp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &m_oldCompMainTexSlot);
    if (m_oldCompMainTexSlot == UINT_MAX) m_oldCompMainTexSlot = 0;
  }

  out.compBlend.Reset();
  if (sh.comp.bytecodeBlob && g_pCompVSBlob) {
    out.compBlend = DX12CreatePresetPSO(
      device, rootSig, compRtvFormat,
      g_pCompVSBlob,
      sh.comp.bytecodeBlob->GetBufferPointer(),
      (UINT)sh.comp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      true, nullptr);  // alphaBlend=true
  }

  DLOG_VERBOSE("DX12: Preset warp PSO: %s (mainTexSlot=%u)", out.warp ? "OK" : "FALLBACK", m_warpMainTexSlot);
  DLOG_VERBOSE("DX12: Preset comp PSO: %s (mainTexSlot=%u)", out.comp ? "OK" : "FALLBACK", m_compMainTexSlot);
  if (out.oldWarp) DLOG_VERBOSE("DX12: Old warp PSO: OK (blend transition)");
  if (out.warpBlend) DLOG_VERBOSE("DX12: Warp blend PSO: OK (alpha blend)");
  if (out.oldComp) DLOG_VERBOSE("DX12: Old comp PSO: OK (blend transition)");
  if (out.compBlend) DLOG_VERBOSE("DX12: Comp blend PSO: OK (alpha blend)");
  if (out.bufferA)
    DebugLogA("DX12: Preset bufferA PSO: OK");
  if (out.bufferB)
    DebugLogA("DX12: Preset bufferB PSO: OK");
}

// The engine's own preset PSOs. Kept as a wrapper rather than renaming the
// eleven m_dx12*PSO members: they have 115 references across six files, and a
// rename of that size would bury the change that matters. Builds into a local
// set and then assigns, so the outgoing PSOs stay alive until the whole new
// set exists -- strictly safer than the previous in-place overwrite, given
// this function's own warning about releasing a PSO the GPU is still using.
void Engine::CreateDX12PresetPSOs() {
  // SEEDED from the current set, not default-constructed.
  //
  // The builder writes each PSO only inside its own conditional branch, so a
  // preset that has no comp shader -- or whose comp shader FAILED to compile --
  // leaves that slot untouched. In place, that kept the previous PSO. Assigning
  // an empty `out` back instead NULLED it, and the next frame drew with a null
  // pipeline state: measured as `noCompPSO=1`, a DRED page fault at VA 0, and a
  // TDR (0x887A0006) the moment a preset with a broken warp shader was loaded.
  //
  // Seeding makes the round trip faithful: anything the builder does not touch
  // comes back as it was, which is exactly what the in-place version did. The
  // earlier claim that building into a temp was "strictly safer" was wrong in
  // precisely this case, and no render comparison could have caught it -- every
  // preset in that set compiles.
  PresetPsoOwned out;
  out.warp      = m_dx12WarpPSO;
  out.oldWarp   = m_dx12OldWarpPSO;
  out.warpBlend = m_dx12WarpBlendPSO;
  out.comp      = m_dx12CompPSO;
  out.oldComp   = m_dx12OldCompPSO;
  out.compBlend = m_dx12CompBlendPSO;
  out.compFloat = m_dx12CompFloatPSO;
  out.bufferA   = m_dx12BufferAPSO;
  out.bufferB   = m_dx12BufferBPSO;
  out.bufferC   = m_dx12BufferCPSO;
  out.bufferD   = m_dx12BufferDPSO;
  CreateDX12PresetPSOs(m_shaders, m_OldShaders, out);
  // The sync that used to sit at the top of the builder. It belongs HERE: the
  // assignments below release the outgoing PSOs, and the GPU may still be
  // executing a command list that references them.
  //
  // WaitForGpu now covers the MIRROR queue as well as the main one, via the
  // waiter the Engine installs on DXContext (#206) -- and this call site is
  // why. A mirror records with THESE PSOs whenever its preset has no custom
  // warp/comp bytecode, because FillPresetPipeline(rc, c) falls back to the
  // engine's set, which is most classic .milk presets. The mirror submits to
  // m_mirrorQueue with its own fence, about which the old main-queue-only wait
  // said nothing, so the assignment below released PSOs an in-flight mirror
  // list still referenced: a GPU page fault at VA 0 on a draw in
  // MirrorWork:\\.\DISPLAYn, then a TDR. Reproduced in roughly one 90-second
  // soak in two, with two independent mirrors and a preset load every 1.2s.
  if (m_lpDX) m_lpDX->WaitForGpu();
  m_dx12WarpPSO      = out.warp;
  m_dx12OldWarpPSO   = out.oldWarp;
  m_dx12WarpBlendPSO = out.warpBlend;
  m_dx12CompPSO      = out.comp;
  m_dx12OldCompPSO   = out.oldComp;
  m_dx12CompBlendPSO = out.compBlend;
  m_dx12CompFloatPSO = out.compFloat;
  m_dx12BufferAPSO   = out.bufferA;
  m_dx12BufferBPSO   = out.bufferB;
  m_dx12BufferCPSO   = out.bufferC;
  m_dx12BufferDPSO   = out.bufferD;
}

// Preprocessor: fix matrix * vector multiplication.
// HLSL requires mul() for matrix-vector multiply; the * operator causes X3020 type mismatch.
// Finds variables declared as float2x2/float3x3/float4x4 and wraps their * operations.
static void FixMatrixVarMultiply(std::string& text) {
  auto isIdent = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  // Phase 1: collect matrix variable names
  static const char* matTypes[] = {
    "float2x2", "float2x3", "float2x4",
    "float3x2", "float3x3", "float3x4",
    "float4x2", "float4x3", "float4x4"
  };
  char matVars[64][64];  // up to 64 matrix variables
  int matVarLens[64];
  int nMatVars = 0;

  {
    const char* base = text.c_str();
    for (auto& mt : matTypes) {
      int mtLen = (int)strlen(mt);
      const char* s = base;
      while ((s = strstr(s, mt)) != NULL) {
        if (s > base && isIdent(s[-1])) { s += mtLen; continue; }
        if (isIdent(s[mtLen])) { s += mtLen; continue; }
        const char* p = s + mtLen;
        while (*p == ' ' || *p == '\t') p++;
        const char* nameStart = p;
        while (isIdent(*p)) p++;
        int nameLen = (int)(p - nameStart);
        if (nameLen > 0 && nameLen < 63) {
          while (*p == ' ' || *p == '\t') p++;
          if (*p != '(') {  // not a function declaration
            memcpy(matVars[nMatVars], nameStart, nameLen);
            matVars[nMatVars][nameLen] = '\0';
            matVarLens[nMatVars] = nameLen;
            nMatVars++;
            if (nMatVars >= 64) break;
          }
        }
        s = p;
      }
      if (nMatVars >= 64) break;
    }
  }

  if (nMatVars == 0) return;

  // Phase 2: replace matVar*ident -> mul(matVar, ident) and ident*matVar -> mul(ident, matVar)
  //
  // Output goes into a std::string that grows as needed. This used to be one
  // malloc(srcLen + 32768) hoisted ABOVE this loop, while the loop runs once per
  // matrix variable -- up to 64 passes, each able to lengthen the text, none of
  // them resizing the buffer or checking it. The write back into the caller's
  // fixed 262144-byte buffer was unbounded too. Both are driven purely by the
  // contents of a preset file, which is a heap overflow reachable by opening a
  // preset someone sent you. See forgejo#18.
  for (int mi = 0; mi < nMatVars; mi++) {
    const char* mv = matVars[mi];
    int mvLen = matVarLens[mi];

    const char* szShaderText = text.c_str();
    int srcLen = (int)text.size();
    std::string out;
    out.reserve(text.size() + 1024);

    for (int i = 0; i < srcLen; ) {
      // Check for word-boundary match of matrix variable name
      if (strncmp(&szShaderText[i], mv, mvLen) == 0 &&
          (i == 0 || !isIdent(szShaderText[i - 1])) &&
          !isIdent(szShaderText[i + mvLen])) {
        // Forward: matVar * ident
        int afterMv = i + mvLen;
        int s = afterMv;
        while (szShaderText[s] == ' ') s++;
        if (szShaderText[s] == '*' && szShaderText[s + 1] != '=') {
          int afterStar = s + 1;
          while (szShaderText[afterStar] == ' ') afterStar++;
          int opStart = afterStar;
          while (isIdent(szShaderText[afterStar])) afterStar++;
          if (afterStar > opStart) {
            // If the identifier is followed by '(' it's a function call - include the args
            if (szShaderText[afterStar] == '(') {
              int depth = 1;
              afterStar++; // skip opening '('
              while (szShaderText[afterStar] && depth > 0) {
                if (szShaderText[afterStar] == '(') depth++;
                else if (szShaderText[afterStar] == ')') depth--;
                afterStar++;
              }
            }
            // Include trailing .swizzle (e.g., n.yzw, func().xyz)
            if (szShaderText[afterStar] == '.') {
              afterStar++; // skip '.'
              while (isIdent(szShaderText[afterStar])) afterStar++;
            }
            // Write: mul(matVar, operand)
            out.append("mul(", 4);
            out.append(mv, mvLen);
            out.append(", ", 2);
            out.append(&szShaderText[opStart], afterStar - opStart);
            out.push_back(')');
            i = afterStar;
            continue;
          }
        }
        // Reverse: check if preceded by ident * matVar
        if (i > 0) {
          int bk = i;
          while (bk > 0 && szShaderText[bk - 1] == ' ') bk--;
          if (bk > 0 && szShaderText[bk - 1] == '*' && (bk < 2 || szShaderText[bk - 2] != '=')) {
            int starIdx = bk - 1;
            int opEnd = starIdx;
            while (opEnd > 0 && szShaderText[opEnd - 1] == ' ') opEnd--;
            int opStart = opEnd;
            while (opStart > 0 && isIdent(szShaderText[opStart - 1])) opStart--;
            // Include preceding ident.swizzle pattern (e.g., n.yzw -> capture full "n.yzw")
            if (opStart > 1 && szShaderText[opStart - 1] == '.') {
              int dotPos = opStart - 1;
              int identStart = dotPos;
              while (identStart > 0 && isIdent(szShaderText[identStart - 1])) identStart--;
              if (identStart < dotPos)
                opStart = identStart;  // include "n." prefix
            }
            // The rewind below removes the already-written "operand * " from the
            // output. Guarded because it is the one place that moves the write
            // cursor backwards, and a rewind past the start would be silent.
            if (opEnd > opStart && out.size() >= (size_t)(i - opStart)) {
              out.resize(out.size() - (size_t)(i - opStart));
              out.append("mul(", 4);
              out.append(&szShaderText[opStart], opEnd - opStart);
              out.append(", ", 2);
              out.append(mv, mvLen);
              out.push_back(')');
              i += mvLen;
              continue;
            }
          }
        }
        // No match - copy as-is
        out.append(mv, mvLen);
        i += mvLen;
      } else {
        out.push_back(szShaderText[i++]);
      }
    }
    text.swap(out);
  }
}

// Preprocessor: fix self-referencing variable redeclarations.
// Some presets redeclare variables with self-initialization like:
//   float3 ret1, ...;    (initial declaration)
//   ...
//   float3 ret1 = ret1;  (redeclaration with self-init)
// The DX9 compiler (d3dx9_43.dll) handles this by initializing the new variable
// from the outer scope. D3DCompile (d3dcompiler_47.dll) sees the RHS as the new
// (uninitialized) variable, causing X4000 error. Fix by removing the redundant
// type specifier, converting `float3 ret1 = ret1;` to `ret1 = ret1;` (a no-op).
// The one rewriter that cannot grow the text: it blanks the redundant type
// keyword with spaces IN PLACE, so the length never changes and it never needed
// a growth buffer. Takes a std::string purely so the whole chain has one
// signature.
static void FixSelfRedeclarations(std::string& text) {
  char* szShaderText = &text[0];
  static const char* types[] = {
    "float4x4", "float4x3", "float3x3", "float3x4",
    "float4", "float3", "float2", "float",
    "half4", "half3", "half2", "half",
    "int4", "int3", "int2", "int",
    "uint4", "uint3", "uint2", "uint",
    "double4", "double3", "double2", "double",
  };

  auto isIdentChar = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  for (auto& type : types) {
    int typeLen = (int)strlen(type);
    char* p = szShaderText;

    while ((p = strstr(p, type)) != nullptr) {
      // Ensure word boundary before the type
      if (p > szShaderText && isIdentChar(*(p - 1))) { p += typeLen; continue; }
      // Ensure word boundary after the type
      if (isIdentChar(p[typeLen])) { p += typeLen; continue; }

      char* typeStart = p;
      char* afterType = p + typeLen;

      // Skip whitespace after type
      char* ws1 = afterType;
      while (*ws1 == ' ' || *ws1 == '\t') ws1++;
      if (!isIdentChar(*ws1) || (*ws1 >= '0' && *ws1 <= '9')) { p = ws1; continue; }

      // Extract variable name
      char* nameStart = ws1;
      char* nameEnd = nameStart;
      while (isIdentChar(*nameEnd)) nameEnd++;
      int nameLen = (int)(nameEnd - nameStart);
      if (nameLen <= 0 || nameLen > 64) { p = nameEnd; continue; }

      // Skip whitespace after name
      char* ws2 = nameEnd;
      while (*ws2 == ' ' || *ws2 == '\t') ws2++;

      // Must have '=' (not '==')
      if (*ws2 != '=' || ws2[1] == '=') { p = ws2; continue; }
      ws2++; // skip '='

      // Skip whitespace after '='
      while (*ws2 == ' ' || *ws2 == '\t') ws2++;

      // Check if RHS starts with the same variable name at word boundary
      if (strncmp(ws2, nameStart, nameLen) != 0) { p = ws2; continue; }
      if (isIdentChar(ws2[nameLen])) { p = ws2; continue; }

      // Skip whitespace after RHS name
      char* afterRHS = ws2 + nameLen;
      while (*afterRHS == ' ' || *afterRHS == '\t') afterRHS++;

      // Must end with ';' — this is `type name = name;`
      if (*afterRHS != ';') { p = afterRHS; continue; }

      // Found a self-referencing redeclaration! Blank out the type keyword with spaces.
      DLOG_INFO("FixSelfRedeclarations: removing redundant '%.*s' from '%.*s'",
                typeLen, type, (int)(afterRHS + 1 - typeStart), typeStart);
      for (int i = 0; i < typeLen; i++)
        typeStart[i] = ' ';

      p = afterRHS + 1;
    }
  }
}

// Preprocessor: rename local variables that shadow HLSL built-in functions.
// Many MilkDrop presets use patterns like `float2 pow = float2(pow(x,y)...)` which
// fails in SM3.0+ because the local variable shadows the intrinsic. We rename the
// variable (non-function-call occurrences) to `_mw_<name>` while leaving `<name>(` calls intact.
static void FixShadowedBuiltins(std::string& text) {
  static const char* builtins[] = {
    "pow", "mul", "sin", "cos", "tan", "exp", "log", "dot",
    "abs", "min", "max", "step", "lerp", "frac", "sqrt",
    "floor", "ceil", "round", "sign", "clamp", "saturate",
    "normalize", "length", "distance", "cross", "clip",
    "line",   // geometry shader primitive type keyword
    "point",  // geometry shader primitive type keyword
  };
  static const int nBuiltins = sizeof(builtins) / sizeof(builtins[0]);

  auto isIdentChar = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  // Type keywords that precede a variable declaration
  auto isTypeKeyword = [](const char* p) -> bool {
    // Check backwards from a position to see if a type keyword precedes it
    // We check common HLSL types: float, float2, float3, float4, int, int2, int3, int4, half, etc.
    static const char* types[] = {
      "float4x4", "float4x3", "float3x3", "float3x4",
      "float4", "float3", "float2", "float",
      "half4", "half3", "half2", "half",
      "int4", "int3", "int2", "int",
      "uint4", "uint3", "uint2", "uint",
      "double4", "double3", "double2", "double",
    };
    for (auto& t : types) {
      int tlen = (int)strlen(t);
      if (!strncmp(p, t, tlen) && !isalnum((unsigned char)p[tlen]) && p[tlen] != '_')
        return true;
    }
    return false;
  };

  // Output grows as needed. This used to be one malloc(srcLen + 32768) hoisted
  // above a loop that runs once per built-in -- 28 passes, each able to lengthen
  // the text by 4 chars per occurrence, none of them resizing or checking. See
  // forgejo#18.
  for (int bi = 0; bi < nBuiltins; bi++) {
    const char* szShaderText = text.c_str();
    int srcLen = (int)text.size();
    const char* name = builtins[bi];
    int nameLen = (int)strlen(name);

    // Phase 1: detect if this built-in is shadowed (declared as a variable)
    bool shadowed = false;
    for (const char* s = szShaderText; *s; s++) {
      // Look for type keyword followed by whitespace then the built-in name
      if (!isTypeKeyword(s)) continue;
      // Skip past the type keyword
      const char* afterType = s;
      while (*afterType && (isIdentChar(*afterType))) afterType++;
      // Must have whitespace after type
      if (*afterType != ' ' && *afterType != '\t' && *afterType != '\n' && *afterType != '\r') continue;
      while (*afterType == ' ' || *afterType == '\t' || *afterType == '\n' || *afterType == '\r') afterType++;
      // Check if the identifier here is our built-in name
      if (strncmp(afterType, name, nameLen) == 0 && !isIdentChar(afterType[nameLen])) {
        // Check that it's not a function call (name followed by '(')
        const char* afterName = afterType + nameLen;
        while (*afterName == ' ' || *afterName == '\t') afterName++;
        if (*afterName != '(') {
          shadowed = true;
          break;
        }
      }
    }

    if (!shadowed) continue;

    // Phase 2: rename all non-function-call occurrences of this name
    std::string out;
    out.reserve(text.size() + 1024);
    for (int i = 0; i < srcLen; ) {
      // Check for word-boundary match of the built-in name
      if (strncmp(&szShaderText[i], name, nameLen) == 0 &&
          (i == 0 || !isIdentChar(szShaderText[i - 1])) &&
          !isIdentChar(szShaderText[i + nameLen])) {
        // Check if this is a function call: name followed by optional whitespace then '('
        const char* after = &szShaderText[i + nameLen];
        while (*after == ' ' || *after == '\t') after++;
        if (*after == '(') {
          // Function call — keep original name
          out.append(name, nameLen);
          i += nameLen;
        } else {
          // Variable reference — rename to _mw_<name>
          out.append("_mw_", 4);
          out.append(name, nameLen);
          i += nameLen;
        }
      } else {
        out.push_back(szShaderText[i++]);
      }
    }
    text.swap(out);
  }
}

// MilkDrop injects `#define rad _rad_ang.x` / `#define ang _rad_ang.y` so presets can use
// the mesh radial/angle builtins. User shaders that declare `float rad = ...` then expand to
// `float _rad_ang.x = ...`, which HLSL reports as X3036 "redefinition of formal parameter
// '_rad_ang'" and fails recompile (often looks like a crash when switching presets).
// If a local declaration of rad/ang is detected, rename those identifiers to _mw_rad/_mw_ang
// (skipping #define lines and the PS formal `_rad_ang`).
static void FixMilkdropRadAngShadowing(std::string& text) {
  if (text.empty()) return;

  auto isIdentChar = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  auto isTypeKeyword = [](const char* p) -> bool {
    static const char* types[] = {
      "float4x4", "float4x3", "float3x3", "float3x4",
      "float4", "float3", "float2", "float",
      "half4", "half3", "half2", "half",
      "int4", "int3", "int2", "int",
      "uint4", "uint3", "uint2", "uint",
    };
    for (auto& t : types) {
      int tlen = (int)strlen(t);
      if (!strncmp(p, t, tlen) && !isalnum((unsigned char)p[tlen]) && p[tlen] != '_')
        return true;
    }
    return false;
  };

  static const char* names[] = { "rad", "ang" };
  static const char* renames[] = { "_mw_rad", "_mw_ang" };

  // Output grows as needed. The buffer this replaced was sized once, above
  // a loop that lengthens the text on every pass. See forgejo#18.

  for (int ni = 0; ni < 2; ni++) {
    const char* szShaderText = text.c_str();
    int srcLen = (int)text.size();
    const char* name = names[ni];
    const char* rename = renames[ni];
    int nameLen = (int)strlen(name);
    int renameLen = (int)strlen(rename);

    // Detect local declaration: type keyword + whitespace + name (not a function)
    bool shadowed = false;
    for (const char* s = szShaderText; *s; s++) {
      if (!isTypeKeyword(s)) continue;
      const char* afterType = s;
      while (*afterType && isIdentChar(*afterType)) afterType++;
      if (*afterType != ' ' && *afterType != '\t' && *afterType != '\n' && *afterType != '\r') continue;
      while (*afterType == ' ' || *afterType == '\t' || *afterType == '\n' || *afterType == '\r') afterType++;
      if (strncmp(afterType, name, nameLen) == 0 && !isIdentChar(afterType[nameLen])) {
        const char* afterName = afterType + nameLen;
        while (*afterName == ' ' || *afterName == '\t') afterName++;
        if (*afterName != '(') {
          shadowed = true;
          break;
        }
      }
    }
    if (!shadowed) continue;

    DLOG_VERBOSE("FixMilkdropRadAngShadowing: renaming local '%s' -> '%s'", name, rename);

    std::string out;
    out.reserve(text.size() + 1024);
    for (int i = 0; i < srcLen; ) {
      // Skip whole #define lines so we don't rename the macro name itself
      if (szShaderText[i] == '#' && (i == 0 || szShaderText[i - 1] == '\n' || szShaderText[i - 1] == '\r')) {
        while (i < srcLen && szShaderText[i] != '\n')
          out.push_back(szShaderText[i++]);
        continue;
      }

      if (strncmp(&szShaderText[i], name, nameLen) == 0 &&
          (i == 0 || !isIdentChar(szShaderText[i - 1])) &&
          !isIdentChar(szShaderText[i + nameLen])) {
        // Leave function-call form alone (rare for rad/ang)
        const char* after = &szShaderText[i + nameLen];
        while (*after == ' ' || *after == '\t') after++;
        if (*after == '(') {
          out.append(name, nameLen);
          i += nameLen;
        } else {
          out.append(rename, renameLen);
          i += nameLen;
        }
      } else {
        out.push_back(szShaderText[i++]);
      }
    }
    text.swap(out);
  }
}

// Preprocessor: a local that shadows a built-in MACRO and reads it in its own
// initialiser.
//
// 25 presets across one collection carry the identical copy-pasted line:
//
//     float aspect = aspect.x / aspect.y;
//
// `aspect` is not a variable, it is `#define aspect _c0` -- in MDropDX12,
// Milkwave and MilkDrop 3 alike. The line therefore expands to
//
//     float _c0 = _c0.x / _c0.y;
//
// which redeclares the engine's float4 constant register as a local SCALAR.
// HLSL allows `.x` on a scalar as a swizzle, so `aspect.x` compiles; `.y` does
// not, so only the second subscript errors:
//
//     error X3018: invalid subscript 'y'
//
// A four-line fxc repro fails identically under ps_3_0 and ps_5_0, so these
// presets are broken in every engine and always have been -- this is a
// compatibility shim for a circulating idiom, not a bug in our assembly. Any
// accompanying '_safe_denom: no matching 1 parameter function' is fallout: the
// failed subscript leaves the argument type unresolvable.
//
// What the author meant is unambiguous: take the built-in float4, divide x by
// y, and use `aspect` as that scalar afterwards. So the DECLARATION and every
// use AFTER it are renamed, while the initialiser is left bound to the macro.
// Renaming the initialiser too -- which is what the rad/ang fixer below would
// do -- just yields `float _mw_aspect = _mw_aspect.x / _mw_aspect.y;`, still
// self-referential and still broken.
//
// Scoped to the names actually observed. `time` is also shadowed in the
// corpus, but by Shadertoy-style code that does not read `time` in its own
// initialiser, so it is a different situation and is left alone.
static void FixSelfReferentialMacroShadow(std::string& text) {
  if (text.empty()) return;
  char* szShaderText = &text[0];

  auto isIdentChar = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  auto isTypeKeyword = [&isIdentChar](const char* p) -> bool {
    static const char* types[] = {
      "float4", "float3", "float2", "float",
      "half4", "half3", "half2", "half",
      "int4", "int3", "int2", "int",
    };
    for (auto& ty : types) {
      size_t tlen = strlen(ty);
      if (!strncmp(p, ty, tlen) && !isIdentChar(p[tlen])) return true;
    }
    return false;
  };

  static const char* const kNames[]   = { "aspect" };
  static const char* const kRenames[] = { "_mw_aspect" };

  for (int ni = 0; ni < (int)(sizeof(kNames) / sizeof(kNames[0])); ni++) {
    const char* name   = kNames[ni];
    const char* rename = kRenames[ni];
    const size_t nameLen = strlen(name);
    const size_t renameLen = strlen(rename);

    // Find `<type> name` where name is not a function call, and the statement
    // reads `name` again before its terminating ';'. Anything else is either
    // not a shadow or a shadow that compiles fine on its own.
    char* decl = nullptr;
    for (char* s = szShaderText; *s; s++) {
      if (s != szShaderText && isIdentChar(s[-1])) continue;
      if (!isTypeKeyword(s)) continue;
      char* p = s;
      while (*p && isIdentChar(*p)) p++;
      if (*p != ' ' && *p != '\t') continue;
      while (*p == ' ' || *p == '\t') p++;
      if (strncmp(p, name, nameLen) != 0 || isIdentChar(p[nameLen])) continue;
      char* afterName = p + nameLen;
      while (*afterName == ' ' || *afterName == '\t') afterName++;
      if (*afterName != '=') continue;          // no initialiser: nothing to save
      char* semi = strchr(afterName, ';');
      if (!semi) continue;
      // Does the initialiser read the macro?
      bool selfRefs = false;
      for (char* q = afterName; q < semi; q++) {
        if (strncmp(q, name, nameLen) == 0 &&
            !isIdentChar(q[-1]) && !isIdentChar(q[nameLen])) {
          selfRefs = true;
          break;
        }
      }
      if (!selfRefs) continue;
      decl = p;
      break;
    }
    if (!decl) continue;

    char* semi = strchr(decl, ';');
    if (!semi) continue;

    // Output grows as needed. This used to be malloc(srcLen + 32768), with the
    // rename loop below writing into it unchecked -- see forgejo#18.
    std::string out;
    out.reserve(text.size() + 1024);

    // Everything before the declared name, verbatim.
    out.append(szShaderText, (size_t)(decl - szShaderText));
    // The declared name -> renamed.
    out.append(rename, renameLen);
    // The initialiser, verbatim: its `aspect` must stay the macro.
    out.append(decl + nameLen, (size_t)(semi + 1 - (decl + nameLen)));

    // Everything after: rename whole-word uses, leaving #define lines alone.
    for (const char* i = semi + 1; *i; ) {
      if (*i == '#' && (i[-1] == '\n' || i[-1] == '\r')) {
        while (*i && *i != '\n') out.push_back(*i++);
        continue;
      }
      if (strncmp(i, name, nameLen) == 0 &&
          !isIdentChar(i[-1]) && !isIdentChar(i[nameLen])) {
        out.append(rename, renameLen);
        i += nameLen;
        continue;
      }
      out.push_back(*i++);
    }
    text.swap(out);
    szShaderText = &text[0];
    DLOG_VERBOSE("FixSelfReferentialMacroShadow: '%s' -> '%s'", name, rename);
  }
}

// Preprocessor: rename local variables that shadow user-defined functions.
// GLSL allows `vec2 R2D = uv * R2D(100.);` where R2D is both a function and local variable.
// HLSL rejects this with X3005: "identifier represents a variable, not a function".
// We scan for function definitions, then rename variable-use occurrences of the same name.
// Preprocessor: give the preset's OWN globals static linkage (#326).
//
// A preset declares scratch variables at file scope and then writes to them
// from the shader body:
//
//     float near, noise, z, k1, ftu, nshape, nmask, iswater;
//     bool  istube;
//     ...
//     z = q8/uv1.y;
//
// Under DX9 that worked. In SM5 a file-scope variable without `static` is a
// uniform -- it lives in a constant buffer and is read-only -- so the write is
// rejected. With backwards compatibility enabled, which is how this engine
// compiles, the rejection arrives in disguise:
//
//     error X4555: cannot use casts on l-values
//
// and without it as the plainer `error X3025: global variables are implicitly
// constant`. Same cause, and neither message mentions the word the fix needs.
//
// Marking them static also settles a second error from the same root:
//
//     error X3011: 'r3': initial value must be a literal expression
//
// A uniform's initialiser has to be a compile-time constant because it is
// baked into the constant buffer, so `float r3 = sqrt(3);` -- which DX9 folded
// -- is refused. A static has no such restriction; it is initialised at entry.
// Presets that already say `static` prove it: `martin - the sewers` writes
// `static float trad = 0.008, tlen = q7;` in the same shader that fails, and
// that line has always compiled.
//
// Scoped to the preset's own text via `from`, and that is the whole safety
// argument. Everything before it is the engine's prelude, where the globals ARE
// uniforms -- q1..q32, the texsize constants, _c0 -- and making any of those
// static would cut it off from the values the CPU writes each frame.
//
// Only a declaration is touched: a name followed by '(' is a function, a name
// followed by ':' carries a semantic and belongs to the pipeline, and a
// statement whose first token is static/const/uniform/struct is left as it is
// because its first token is not a type. Samplers and textures never match for
// the same reason, which matters -- a static sampler does not compile.
//
// NOT EVERY PRESET GLOBAL IS SCRATCH, which is the trap here and it was caught
// by comparing compiled output rather than by reading the code. A preset
// declares
//
//     float4 texsize_bw3;
//
// and the ENGINE fills that in: CacheParams reflects over the compiled
// constants and binds anything named texsize_* to the dimensions of the matching
// texture. Made static, it is cut off from that and reads as zero -- which
// compiles, and silently folds away every expression it feeds. On
// `Nivush - Bouncing Sounds` the constant buffer fell from 115 vectors to 25
// and a whole block of arithmetic vanished. No error, just a different picture.
//
// So a global is made static only when it CANNOT be one of those:
//
//   * the preset assigns to it somewhere -- engine-supplied constants are
//     read-only, and a write is the very thing X4555/X3025 complain about; or
//   * its initialiser is not a constant expression, which no engine-bound
//     constant has and which is exactly what X3011 rejects.
//
// Both conditions describe a variable the preset owns. Names the engine binds
// by reflection are excluded outright as well, because that contract is known
// rather than inferred and costs one comparison to honour.
static void MakePresetGlobalsStatic(std::string& text, size_t from) {
  static const char* kTypes[] = {
    "float", "float2", "float3", "float4",
    "float2x2", "float3x3", "float4x4",
    "float2x3", "float3x2", "float2x4", "float4x2", "float3x4", "float4x3",
    "half", "half2", "half3", "half4",
    "double", "double2", "double3", "double4",
    "int", "int2", "int3", "int4",
    "uint", "uint2", "uint3", "uint4",
    "bool", "bool2", "bool3", "bool4",
  };
  auto isIdent = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };
  auto isTypeName = [&](const std::string& t) {
    for (const char* ty : kTypes) if (t == ty) return true;
    return false;
  };

  // Names CacheParams binds by reflection, from the shader's own constant
  // table. The preset declares them; the engine supplies the value.
  auto engineBound = [](const std::string& n) {
    return n.compare(0, 8, "texsize_") == 0 ||
           n.compare(0, 4, "rot_")     == 0 ||
           n == "rand_frame" || n == "rand_preset" ||
           (!n.empty() && n[0] == '_');
  };

  // Is `name` written to anywhere after `from`? Allows a swizzle or an index
  // in between, and rejects the comparison operators that also contain '='.
  auto isAssigned = [&](const std::string& body, size_t start, const std::string& name) {
    for (size_t p = body.find(name, start); p != std::string::npos;
         p = body.find(name, p + 1)) {
      if (p > 0 && isIdent(body[p - 1])) continue;          // part of a longer name
      size_t q = p + name.size();
      if (q < body.size() && isIdent(body[q])) continue;
      // step over `.xyz` and `[i]`
      for (;;) {
        while (q < body.size() && (unsigned char)body[q] <= ' ') q++;
        if (q < body.size() && body[q] == '.') {
          q++;
          while (q < body.size() && isIdent(body[q])) q++;
        } else if (q < body.size() && body[q] == '[') {
          int d = 0;
          while (q < body.size()) {
            if (body[q] == '[') d++;
            else if (body[q] == ']') { q++; if (--d == 0) break; continue; }
            q++;
          }
        } else break;
      }
      while (q < body.size() && (unsigned char)body[q] <= ' ') q++;
      if (q >= body.size()) continue;
      const char c0 = body[q];
      const char c1 = (q + 1 < body.size()) ? body[q + 1] : 0;
      if (c0 == '=' && c1 != '=') return true;                       // x =
      if ((c0 == '+' || c0 == '-') && c1 == c0) return true;         // x++ / x--
      if ((c0 == '+' || c0 == '-' || c0 == '*' || c0 == '/' ||
           c0 == '%') && c1 == '=') return true;                     // x += ...
    }
    return false;
  };

  if (from >= text.size()) return;

  std::string out = text.substr(0, from);
  out.reserve(text.size() + 256);

  const size_t n = text.size();
  int paren = 0, depth = 0, changed = 0;
  char prevSig = 0;

  for (size_t i = from; i < n; ) {
    const char c = text[i];

    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n') out += text[i++];
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      out += text[i++]; out += text[i++];
      while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/'))
        out += text[i++];
      if (i < n) { out += text[i++]; if (i < n) out += text[i++]; }
      continue;
    }
    // A preprocessor line is not a declaration however it starts. It DOES end
    // a statement though, and forgetting that is what made this miss
    // `float r3 = sqrt(3);` when it followed `#define hue_shader ...` with no
    // semicolon between them: prevSig was still the 'z' of `.xyz`, so the next
    // line did not look like the start of one.
    if (c == '#' && (prevSig == 0 || out.empty() || out.back() == '\n')) {
      while (i < n && text[i] != '\n') out += text[i++];
      prevSig = ';';
      continue;
    }

    if (c == '(') paren++;
    else if (c == ')') { if (paren > 0) paren--; }
    else if (c == '{') depth++;
    else if (c == '}') { if (depth > 0) depth--; }

    const bool atGlobalStatement =
        (paren == 0 && depth == 0 &&
         (prevSig == 0 || prevSig == ';' || prevSig == '}' || prevSig == '{'));

    if (atGlobalStatement && isIdent(c) && (i == from || !isIdent(text[i - 1]))) {
      size_t e = i;
      while (e < n && isIdent(text[e])) e++;
      const std::string tok = text.substr(i, e - i);

      bool isType = false;
      for (const char* ty : kTypes)
        if (tok == ty) { isType = true; break; }

      if (isType) {
        size_t a = e;
        while (a < n && (unsigned char)text[a] <= ' ') a++;
        if (a < n && isIdent(text[a])) {
          size_t ne = a;
          while (ne < n && isIdent(text[ne])) ne++;
          size_t after = ne;
          while (after < n && (unsigned char)text[after] <= ' ') after++;
          // ',' ';' '=' '[' -> a variable. '(' is a function, ':' a semantic.
          if (after < n && (text[after] == ',' || text[after] == ';' ||
                            text[after] == '=' || text[after] == '[')) {
            // Find the end of the statement, then weigh every declarator in it.
            size_t semi = std::string::npos;
            int dp = 0;
            for (size_t s = a; s < n; s++) {
              const char d = text[s];
              if (d == '(' || d == '[') dp++;
              else if (d == ')' || d == ']') dp--;
              else if (dp == 0 && d == ';') { semi = s; break; }
              else if (dp == 0 && (d == '{' || d == '}')) break;
            }

            bool qualifies = false, blocked = (semi == std::string::npos);
            for (size_t s = a; semi != std::string::npos && s < semi; ) {
              while (s < semi && (unsigned char)text[s] <= ' ') s++;
              size_t nb = s;
              while (s < semi && isIdent(text[s])) s++;
              const std::string nm = text.substr(nb, s - nb);

              // The initialiser, if any, up to the next top-level comma.
              size_t init = std::string::npos, t = s;
              int d2 = 0;                 // int: '(' /')' can go unbalanced
              for (; t < semi; t++) {
                if (text[t] == '(' || text[t] == '[') d2++;
                else if (text[t] == ')' || text[t] == ']') d2--;
                else if (d2 == 0 && text[t] == ',') break;
                else if (d2 == 0 && text[t] == '=' && init == std::string::npos)
                  init = t + 1;
              }

              if (nm.empty()) { s = t + 1; continue; }
              if (engineBound(nm)) { blocked = true; break; }

              if (!qualifies) {
                if (isAssigned(text, semi, nm)) qualifies = true;
                else if (init != std::string::npos) {
                  // Non-constant initialiser: any identifier that is not a
                  // type name. `float3(0,0,0)` is constant, `sqrt(3)` and `q7`
                  // are not.
                  for (size_t k = init; k < t && !qualifies; ) {
                    if (!isIdent(text[k])) { k++; continue; }
                    size_t kb = k;
                    while (k < t && isIdent(text[k])) k++;
                    const std::string tok2 = text.substr(kb, k - kb);
                    if (tok2.empty() || (tok2[0] >= '0' && tok2[0] <= '9')) continue;
                    if (!isTypeName(tok2)) qualifies = true;
                  }
                }
              }
              s = t + 1;
            }

            if (qualifies && !blocked) {
              out += "static ";
              changed++;
            }
          }
        }
      }
    }

    if ((unsigned char)c > ' ') prevSig = c;
    out += c;
    i++;
  }

  if (changed) {
    DLOG_INFO("MakePresetGlobalsStatic: %d preset global declaration(s)", changed);
    text.swap(out);
  }
}

// Preprocessor: zero-initialize bare local declarations (#230).
//
// GLSL implementations zero locals in practice, so shaders ported by hand from
// Shadertoy rely on it without saying so. HLSL leaves them undefined. Clouds1b
// is the case that made this worth doing:
//
//     float i,e,g,R,s;
//     float3 q,p,d=float3(...);
//     for(q.zy=float2(-1,-1); i++<99.; )   <-- i is the loop's ONLY bound
//
// so the outer loop's trip count is an undefined register. It is not what
// hangs the GPU on that preset -- measured, a zero-initialized build dies in
// the same five seconds -- but it is undefined behaviour producing visuals
// that need not be the same twice, and it costs nothing to close.
//
// The Shadertoy IMPORTER already does this (engine_shader_import_ui.cpp) and
// says why in its own comment. Presets ported by hand into a .milk file never
// pass through it, which is the gap this closes.
//
// Deliberately per-DECLARATOR, unlike the importer's version, which skips any
// declaration containing '='. That misses `float3 q,p,d=float3(...)` -- the
// initializer belongs to d, while q and p are exactly as undefined as before.
//
// Conservative by construction: only plain scalar/vector declarators made of
// identifier characters are touched. Arrays, anything with parentheses,
// function parameters, struct members and both comment forms are left alone,
// because a wrong rewrite here is a compile error on a preset that used to
// work.
static void FixUninitializedLocals(std::string& text) {
  static const char* kTypes[] = {
    "float4", "float3", "float2", "float",
    "half4", "half3", "half2", "half",
    "int4", "int3", "int2", "int",
    "uint4", "uint3", "uint2", "uint",
    "bool4", "bool3", "bool2", "bool",
  };
  auto isIdent = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
  };

  std::string out;
  out.reserve(text.size() + 256);

  const size_t n = text.size();
  int paren = 0, depth = 0, structDepth = -1;
  char prevSig = 0;             // last significant (non-space) character
  int changed = 0;

  for (size_t i = 0; i < n; ) {
    const char c = text[i];

    // Comments pass through untouched -- a 'float' inside one is prose.
    if (c == '/' && i + 1 < n && text[i + 1] == '/') {
      while (i < n && text[i] != '\n') out += text[i++];
      continue;
    }
    if (c == '/' && i + 1 < n && text[i + 1] == '*') {
      out += text[i++]; out += text[i++];
      while (i < n && !(text[i] == '*' && i + 1 < n && text[i + 1] == '/'))
        out += text[i++];
      if (i < n) { out += text[i++]; if (i < n) out += text[i++]; }
      continue;
    }

    if (c == '(') paren++;
    else if (c == ')') { if (paren > 0) paren--; }
    else if (c == '{') {
      // Remember whether this block is a struct body, so its members are not
      // treated as locals -- `struct S { float a; };` must stay as it is.
      //
      // The tag name has to be stepped over first (#326). Looking only at the
      // token immediately before '{' recognises the anonymous `struct {` and
      // misses every NAMED one, so `struct MarchData { float d; }` had " = 0"
      // appended to each member and the compiler rejected it:
      //
      //     error X3009: 'MarchData::d': struct/class members cannot have
      //                  default values
      size_t b = out.size();
      while (b > 0 && (unsigned char)out[b - 1] <= ' ') b--;
      size_t tag = b;                       // skip an optional `MarchData`
      while (tag > 0 && (isIdent(out[tag - 1]))) tag--;
      if (tag < b) {
        size_t ws = tag;
        while (ws > 0 && (unsigned char)out[ws - 1] <= ' ') ws--;
        if (ws < tag) b = ws;               // there was `struct <tag> {`
      }
      const bool isStruct = (b >= 6 && out.compare(b - 6, 6, "struct") == 0) ||
                            (b >= 5 && out.compare(b - 5, 5, "class") == 0) ||
                            (structDepth >= 0);
      depth++;
      if (isStruct && structDepth < 0) structDepth = depth;
    }
    else if (c == '}') {
      if (structDepth >= 0 && depth == structDepth) structDepth = -1;
      if (depth > 0) depth--;
    }

    const bool atStatement = (paren == 0 && structDepth < 0 &&
                              (prevSig == 0 || prevSig == ';' ||
                               prevSig == '{' || prevSig == '}'));
    bool handled = false;

    if (atStatement && (i == 0 || !isIdent(text[i - 1]))) {
      for (const char* ty : kTypes) {
        const size_t tl = strlen(ty);
        if (text.compare(i, tl, ty) != 0) continue;
        if (i + tl < n && (isIdent(text[i + tl]) || text[i + tl] == 'x'))
          continue;                       // float2x2, floaty, ...
        if (i + tl >= n || text[i + tl] != ' ') continue;

        // Find the end of the statement. A '{' at depth 0 means this was a
        // function definition, not a declaration.
        size_t semi = std::string::npos;
        int dp = 0;
        for (size_t s = i + tl; s < n; s++) {
          const char d = text[s];
          if (d == '(' || d == '[') dp++;
          else if (d == ')' || d == ']') dp--;
          else if (dp == 0 && d == ';') { semi = s; break; }
          else if (dp == 0 && (d == '{' || d == '}')) break;
        }
        if (semi == std::string::npos) break;

        // Split the declarator list on top-level commas and give a bare one
        // "= 0". HLSL promotes the scalar across a vector, so one form works
        // for every type here.
        const std::string decls = text.substr(i + tl, semi - (i + tl));
        std::string rebuilt;
        size_t start = 0;
        int dp2 = 0;
        bool safe = true;
        std::vector<std::string> parts;
        for (size_t s = 0; s <= decls.size(); s++) {
          if (s == decls.size() || (dp2 == 0 && decls[s] == ',')) {
            parts.push_back(decls.substr(start, s - start));
            start = s + 1;
          } else if (decls[s] == '(' || decls[s] == '[') dp2++;
          else if (decls[s] == ')' || decls[s] == ']') dp2--;
        }
        for (auto& p : parts) {
          if (p.find('=') != std::string::npos) continue;   // already has one
          size_t a = 0, b2 = p.size();
          while (a < b2 && (unsigned char)p[a] <= ' ') a++;
          while (b2 > a && (unsigned char)p[b2 - 1] <= ' ') b2--;
          if (a >= b2) { safe = false; break; }
          for (size_t k = a; k < b2; k++)
            if (!isIdent(p[k])) { safe = false; break; }   // array, call, ...
          if (!safe) break;
          p.insert(b2, " = 0");
        }
        if (!safe) break;
        for (size_t k = 0; k < parts.size(); k++) {
          if (k) rebuilt += ',';
          rebuilt += parts[k];
        }
        if (rebuilt != decls) changed++;
        out.append(ty, tl);
        out += rebuilt;
        out += ';';
        i = semi + 1;
        prevSig = ';';
        handled = true;
        break;
      }
    }

    if (handled) continue;
    out += c;
    if ((unsigned char)c > ' ') prevSig = c;
    i++;
  }

  if (changed) {
    DLOG_INFO("FixUninitializedLocals: zero-initialized %d local declaration(s)",
              changed);
    text.swap(out);
  }
}

static void FixShadowedUserFunctions(std::string& text) {
  char* szShaderText = &text[0];
  auto isIdentChar = [](char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };

  // Type keywords that can precede a function definition or variable declaration
  static const char* types[] = {
    "float4x4", "float4x3", "float3x4", "float3x3", "float2x2",
    "float4", "float3", "float2", "float",
    "half4", "half3", "half2", "half",
    "int4", "int3", "int2", "int",
    "uint4", "uint3", "uint2", "uint",
    "double4", "double3", "double2", "double",
    "void", "bool",
  };
  auto isTypeKeyword = [&](const char* p) -> bool {
    for (auto& t : types) {
      int tlen = (int)strlen(t);
      if (!strncmp(p, t, tlen) && !isalnum((unsigned char)p[tlen]) && p[tlen] != '_')
        return true;
    }
    return false;
  };

  int srcLen = (int)strlen(szShaderText);

  // Phase 1: collect user-defined function names.
  // Pattern: typeKeyword whitespace identifier '(' — where identifier is NOT a known keyword.
  struct FuncName { char name[128]; int len; };
  FuncName funcNames[64];
  int nFuncs = 0;

  for (const char* s = szShaderText; *s && nFuncs < 64; s++) {
    if (!isTypeKeyword(s)) continue;
    const char* afterType = s;
    while (*afterType && isIdentChar(*afterType)) afterType++;
    if (*afterType != ' ' && *afterType != '\t' && *afterType != '\n' && *afterType != '\r') continue;
    while (*afterType == ' ' || *afterType == '\t' || *afterType == '\n' || *afterType == '\r') afterType++;
    // Read identifier
    const char* idStart = afterType;
    while (isIdentChar(*afterType)) afterType++;
    int idLen = (int)(afterType - idStart);
    if (idLen == 0 || idLen >= 127) continue;
    // Must be followed by '('
    const char* afterId = afterType;
    while (*afterId == ' ' || *afterId == '\t') afterId++;
    if (*afterId != '(') continue;
    // Skip common keywords that look like functions but aren't
    if (idLen == 2 && !strncmp(idStart, "if", 2)) continue;
    if (idLen == 3 && !strncmp(idStart, "for", 3)) continue;
    if (idLen == 5 && !strncmp(idStart, "while", 5)) continue;
    // Check it's not already a built-in (handled by FixShadowedBuiltins)
    // Store the name
    char funcName[128];
    memcpy(funcName, idStart, idLen);
    funcName[idLen] = 0;
    // Deduplicate
    bool dup = false;
    for (int i = 0; i < nFuncs; i++) {
      if (funcNames[i].len == idLen && !strcmp(funcNames[i].name, funcName)) { dup = true; break; }
    }
    if (!dup) {
      memcpy(funcNames[nFuncs].name, funcName, idLen + 1);
      funcNames[nFuncs].len = idLen;
      nFuncs++;
    }
  }

  if (nFuncs == 0) return;

  // Phase 2: for each function name, check if it's also used as a variable.
  //
  // Output grows as needed. The buffer this replaced was sized once, above a
  // loop that runs once per collected function name and lengthens the text by
  // 4 chars per occurrence on every pass. See forgejo#18.
  for (int fi = 0; fi < nFuncs; fi++) {
    szShaderText = &text[0];
    srcLen = (int)text.size();
    const char* name = funcNames[fi].name;
    int nameLen = funcNames[fi].len;

    // Check if this name is also declared as a variable (type name without '(' after)
    bool shadowed = false;
    for (const char* s = szShaderText; *s; s++) {
      if (!isTypeKeyword(s)) continue;
      const char* afterType = s;
      while (*afterType && isIdentChar(*afterType)) afterType++;
      if (*afterType != ' ' && *afterType != '\t' && *afterType != '\n' && *afterType != '\r') continue;
      while (*afterType == ' ' || *afterType == '\t' || *afterType == '\n' || *afterType == '\r') afterType++;
      if (strncmp(afterType, name, nameLen) == 0 && !isIdentChar(afterType[nameLen])) {
        const char* afterName = afterType + nameLen;
        while (*afterName == ' ' || *afterName == '\t') afterName++;
        if (*afterName != '(') {
          shadowed = true;
          break;
        }
      }
    }

    if (!shadowed) continue;

    DLOG_INFO("FixShadowedUserFunctions: renaming variable '%s' -> '_mw_%s'", name, name);

    // Rename non-function-call occurrences (same logic as FixShadowedBuiltins)
    std::string out;
    out.reserve(text.size() + 1024);
    for (int i = 0; i < srcLen; ) {
      if (strncmp(&szShaderText[i], name, nameLen) == 0 &&
          (i == 0 || !isIdentChar(szShaderText[i - 1])) &&
          !isIdentChar(szShaderText[i + nameLen])) {
        const char* after = &szShaderText[i + nameLen];
        while (*after == ' ' || *after == '\t') after++;
        if (*after == '(') {
          // Function call or definition — keep original name
          out.append(name, nameLen);
          i += nameLen;
        } else {
          // Variable reference — rename to _mw_<name>
          out.append("_mw_", 4);
          out.append(name, nameLen);
          i += nameLen;
        }
      } else {
        out.push_back(szShaderText[i++]);
      }
    }
    text.swap(out);
  }
}

// 1-based line number of byte offset `pos` within `buf`.  Used to record where
// the engine's generated prelude and in-body injections land in the assembled
// shader text, so MapCompiledLineToUserLine() can undo the shift.
static int LineOfOffset(const char* buf, size_t pos) {
  int line = 1;
  for (size_t i = 0; i < pos; i++)
    if (buf[i] == '\n') line++;
  return line;
}

bool Engine::LoadShaderFromMemory(const char* szOrigShaderText, char* szFn, char* szProfile,
  LPD3DXCONSTANTTABLE* ppConstTable, void** ppShader, int shaderType, bool bHardErrors, bool compileOnly,
  LPD3DXBUFFER* ppBytecodeOut, const char* szDiagName) {

  const char szWarpDefines[] = "#define rad _rad_ang.x\n"
    "#define ang _rad_ang.y\n"
    "#define uv _uv.xy\n"
    "#define uv_orig _uv.zw\n";
  const char szCompDefines[] = "#define rad _rad_ang.x\n"
    "#define ang _rad_ang.y\n"
    "#define uv _uv.xy\n"
    "#define uv_orig _uv.xy\n"
    "#define hue_shader _vDiffuse.xyz\n";
  const char szWarpParams[] = "float4 _vDiffuse : COLOR, float4 _uv : TEXCOORD0, float2 _rad_ang : TEXCOORD1, out float4 _return_value : COLOR0";
  const char szCompParams[] = "float4 _vDiffuse : COLOR, float4 _uv : TEXCOORD0, float2 _rad_ang : TEXCOORD1, out float4 _return_value : COLOR0";
  const char szFirstLine[] = "    float3 ret = 0;";

  char szWhichShader[64];
  switch (shaderType) {
  case SHADER_WARP:  CopyToA(szWhichShader, "warp"); break;
  case SHADER_COMP:  CopyToA(szWhichShader, "composite"); break;
  case SHADER_BLUR:  CopyToA(szWhichShader, "blur"); break;
  case SHADER_OTHER: CopyToA(szWhichShader, "(other)"); break;
  default:           CopyToA(szWhichShader, "(unknown)"); break;
  }

  LPD3DXBUFFER pShaderByteCode = NULL;

  *ppShader = NULL;
  *ppConstTable = NULL;

  // DIAG: log original shader text (before include.fx prepend)
  {
    int origLen = szOrigShaderText ? (int)strlen(szOrigShaderText) : 0;
    char preview[301] = {0};
    if (origLen > 0) {
      strncpy_s(preview, sizeof(preview), szOrigShaderText, _TRUNCATE);
      for (int i = 0; i < 300 && preview[i]; i++)
        if (preview[i] < 32 && preview[i] != 0) preview[i] = '|';
    }
    DLOG_VERBOSE("DIAG LoadShader: type=%d(%s) origLen=%d text='%.300s'",
            shaderType, szWhichShader, origLen, preview);
  }

  // Heap-allocate shader buffers — stack arrays would risk overflow with large Shadertoy shaders
  struct ShaderBuf { char* p; ShaderBuf(size_t n) : p((char*)malloc(n)) {} ~ShaderBuf() { free(p); } };
  ShaderBuf _szShaderText(MAX_SHADER_TEXT_LEN), _temp(MAX_SHADER_TEXT_LEN);
  char* szShaderText = _szShaderText.p;
  char* temp = _temp.p;
  if (!szShaderText || !temp) return false;
  int writePos = 0;

  // Every pass below writes into this one fixed MAX_SHADER_TEXT_LEN buffer, and
  // a good half of them GROW the text -- some by a constant, some per
  // occurrence. None of them used to check, which is forgejo#101: the writes
  // ran off the end of the allocation and corrupted the heap, and because heap
  // corruption is raised through __fastfail (dispatched to the kernel, never
  // delivered to user-mode SEH) the process vanished with nothing logged and no
  // exception for the load thread's __try/__except to catch.
  //
  // Report and fail the preset instead. Preset text is attacker-controlled
  // input -- file association, drag-drop, command line -- same framing as
  // forgejo#18 and forgejo#99.
  auto tooLarge = [&](const char* what, size_t needed) -> bool {
    wchar_t errMsg[512];
    swprintf(errMsg, 512,
             L"%hs shader too large after %hs: %zu bytes, limit %d",
             szWhichShader, what, needed, (int)MAX_SHADER_TEXT_LEN);
    dumpmsg(errMsg, LOG_WARN);
    AddError(errMsg, 8.0f, ERR_PRESET, true);
    AutoFlagPresetError(PresetFileForCompileError(), errMsg);
    return false;
  };

  // Bounded "insert `text` at p", the operation every wrapper injection below
  // performs by hand: save the tail, write the insertion over it, put the tail
  // back further along. p is advanced past the insertion, as the hand-written
  // form did. Returns false when the result would not fit, having changed
  // nothing.
  //
  // Callers that record an injected line number must compute it BEFORE calling
  // this, from the same p -- the value is identical either way (the text before
  // p does not move) and p has advanced by the time this returns.
  auto insertAt = [&](char*& p, const char* text, const char* what) -> bool {
    const size_t addLen = strlen(text);
    if (strlen(szShaderText) + addLen + 1 > MAX_SHADER_TEXT_LEN)
      return tooLarge(what, strlen(szShaderText) + addLen + 1);
    strncpy_s(temp, MAX_SHADER_TEXT_LEN, p, _TRUNCATE);
    memcpy(p, text, addLen);
    p[addLen] = 0;
    p += addLen;
    strncpy_s(p, MAX_SHADER_TEXT_LEN - (size_t)(p - szShaderText), temp, _TRUNCATE);
    return true;
  };

  // Bounded "replace everything from p onward with `text`". The closing-brace
  // injections do this rather than inserting, because there is no tail left to
  // keep.
  auto writeTailAt = [&](char* p, const char* text, const char* what) -> bool {
    const size_t addLen = strlen(text);
    if ((size_t)(p - szShaderText) + addLen + 1 > MAX_SHADER_TEXT_LEN)
      return tooLarge(what, (size_t)(p - szShaderText) + addLen + 1);
    memcpy(p, text, addLen + 1);
    return true;
  };

  // paste the universal #include
  strncpy_s(&szShaderText[writePos], MAX_SHADER_TEXT_LEN - (size_t)writePos, m_szShaderIncludeText, _TRUNCATE);  // first, paste in the contents of 'inputs.fx' before the actual shader text.  Has 13's and 10's.
  writePos += m_nShaderIncludeTextLen;

  // Strip include's sampler_rand declarations if the preset declares its own
  // (presets use #define MYSAMP sampler_rand00 + sampler MYSAMP; which after
  //  macro expansion creates a second Texture2D declaration → redefinition error)
  // Only strip if the preset actually declares the texture (contains "Texture2D sampler_randNN"),
  // not if it merely references the sampler by name.
  for (int i = 0; i <= 3; i++) {
    char decl[40];
    sprintf(decl, "Texture2D sampler_rand%02d", i);
    // The name has to END at the match. "Texture2D sampler_rand00" is a prefix
    // of "Texture2D sampler_rand00_smalltiled", and taking the prefix as a hit
    // would strip the include's bare declaration because a preset happened to
    // declare a SUFFIXED one -- a different texture entirely. The strip below
    // had the same flaw in the other direction and it cost four presets.
    const char* found = strstr(szOrigShaderText, decl);
    while (found) {
      const char after = *(found + strlen(decl));
      if (!(isalnum((unsigned char)after) || after == '_')) break;
      found = strstr(found + 1, decl);
    }
    if (found) {
      char fullDecl[44];
      sprintf(fullDecl, "%s;", decl);
      char* pos = strstr(szShaderText, fullDecl);
      if (pos) memset(pos, ' ', strlen(fullDecl));
    }
  }

  // Declare the SUFFIXED random-texture samplers (#305).
  //
  // MilkDrop lets a preset write sampler_rand00_smalltiled, where the part
  // after the digits is a filename hint -- "pick a random texture whose name
  // starts with smalltiled". CacheParams already understands that form: it
  // peels the prefix off and calls PickRandomTexture with it.
  //
  // But the include only ever declared the BARE names, sampler_rand00 through
  // sampler_rand15, so the suffixed identifier was never declared and the
  // shader failed to compile before CacheParams could look at anything:
  //
  //   error X3004: undeclared identifier 'sampler_rand00_smalltiled'
  //
  // Found by sweeping the preset libraries: every compile failure across 2,885
  // presets was this one fault -- one in BeatDrop and four in the folder named
  // "Not working with the Original Milkdrop", which is what those four had been
  // failing on all along.
  //
  // Declaring it is the whole fix. Reflection then reports the texture under
  // its full name and the existing rand handling does the rest.
  {
    std::set<std::string> wanted;
    for (const char* s = strstr(szOrigShaderText, "sampler_rand"); s;
         s = strstr(s, "sampler_rand")) {
      const char* p = s + 12;                       // past "sampler_rand"
      if (!isdigit((unsigned char)p[0]) || !isdigit((unsigned char)p[1]) ||
          p[2] != '_') {
        s = p;                                      // bare form; the include has it
        continue;
      }
      const char* q = p + 3;
      while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
      wanted.insert(std::string(s, q - s));
      s = q;
    }
    for (const std::string& name : wanted) {
      // Not if the preset declares it itself -- that is the same redefinition
      // the bare-name strip exists to avoid, from the other direction.
      //
      // BOTH spellings. Checking only "Texture2D <name>" missed the DX9 form,
      // and the sweep found four presets redefining sampler_rand00_smalltiled
      // for exactly that reason -- a fault introduced by #305 declaring a name
      // the preset had already declared as `sampler <name>;`.
      char own[160], ownDx9[160];
      sprintf(own, "Texture2D %s", name.c_str());
      sprintf(ownDx9, "sampler %s", name.c_str());
      if (strstr(szOrigShaderText, own) || strstr(szOrigShaderText, ownDx9))
        continue;
      char decl[176];
      sprintf(decl, "Texture2D %s;\n", name.c_str());
      strncpy_s(&szShaderText[writePos], MAX_SHADER_TEXT_LEN - (size_t)writePos,
                decl, _TRUNCATE);
      writePos += (int)strlen(decl);
    }
  }

  // Strip Shadertoy-specific texture declarations for non-Shadertoy presets
  // to avoid unused t-register slots and potential reflection noise.
  if (!m_bLoadingShadertoyMode) {
    auto blankDecl = [&](const char* pattern) {
      char* pos = strstr(szShaderText, pattern);
      if (pos && pos < &szShaderText[writePos]) {
        char* end = strchr(pos, ';');
        if (end) memset(pos, ' ', end - pos + 1);
      }
    };
    blankDecl("Texture2D sampler_feedback");
    blankDecl("Texture2D sampler_image");
    blankDecl("Texture2D sampler_bufferB");
    blankDecl("Texture2D sampler_bufferC");
    blankDecl("Texture2D sampler_bufferD");
    // NOTE: sampler_audio is NOT blanked — get_fft() functions in include.fx need it
    blankDecl("Texture2D sampler_noise_lq_st");
    blankDecl("Texture2D sampler_noise_mq_st");
    blankDecl("Texture2D sampler_noise_hq_st");
    blankDecl("Texture3D sampler_noisevol_lq_st");
    blankDecl("Texture3D sampler_noisevol_hq_st");
  }

  // paste in any custom #defines for this shader type
  if (shaderType == SHADER_WARP && szProfile[0] == 'p') {
    strncpy_s(&szShaderText[writePos], MAX_SHADER_TEXT_LEN - (size_t)writePos, szWarpDefines, _TRUNCATE);
    writePos += lstrlenA(szWarpDefines);
  }
  else if (shaderType == SHADER_COMP && szProfile[0] == 'p') {
    strncpy_s(&szShaderText[writePos], MAX_SHADER_TEXT_LEN - (size_t)writePos, szCompDefines, _TRUNCATE);
    writePos += lstrlenA(szCompDefines);
  }
  // Shadertoy shaders: #undef MilkDrop Q-variable macros (q1..q32 → _qa.x etc.)
  // to avoid collisions with local GLSL variable names like "float3 q2 = abs(p2);"
  // which would expand to "float3 _qa.y = abs(p2);" and fail to compile.
  if (m_bLoadingShadertoyMode) {
    for (int qi = 1; qi <= 32; qi++) {
      char undef[20];
      int n = sprintf(undef, "#undef q%d\n", qi);
      memcpy(&szShaderText[writePos], undef, n);
      writePos += n;
    }
    szShaderText[writePos] = 0;
  }

  // paste in the shader itself - converting LCC's to 13+10's.
  // avoid lstrcpy b/c it might not handle the linefeed stuff...?
  int shaderStartPos = writePos;

  // Record where the engine's prelude ends so the Preset Editor can map
  // D3DCompile line numbers back to the lines the user actually typed.
  m_shaderInjectedLines.clear();
  m_nShaderPreludeLines = LineOfOffset(szShaderText, (size_t)shaderStartPos) - 1;

  // The assembly above and the copy below share ONE MAX_SHADER_TEXT_LEN buffer,
  // and the preset body is only capped at MAX_SHADER_TEXT_LEN by itself -- see
  // LoadMilk3Preset's strncpy_s(..., _TRUNCATE) and its .milk/.milk2 siblings.
  // So a body near that cap plus the engine's own prelude (the ~10KB include,
  // the warp/comp #defines, and 32 "#undef qN" lines for Shadertoy) does not
  // fit, and this loop wrote past the allocation with nothing checking it --
  // heap corruption, raised through __fastfail, which is dispatched to the
  // kernel and never delivered to user-mode SEH. That is why forgejo#101 saw a
  // process vanish with "LoadMilk3: compile start" as the last line in the log
  // and no crash diagnostic: the load thread's __try/__except had no exception
  // to catch.
  //
  // Measured on the .milk3 path at 262144: a 233682-byte image shader compiled
  // and applied normally in 907ms, 253332 killed the process. The cliff is
  // MAX_SHADER_TEXT_LEN minus the prelude, not MAX_SHADER_TEXT_LEN.
  //
  // The length is counted exactly rather than bounded at 2x. Each
  // LINEFEED_CONTROL_CHAR becomes CR+LF, so the worst case really is double --
  // but a shader made entirely of newlines is not a thing, and assuming the
  // worst case would refuse bodies that fit with room to spare. The shaders
  // this protects are by definition the large ones, which is exactly where a
  // needless rejection would be felt.
  {
    size_t bodyLen = 0;
    for (const char* s = szOrigShaderText; *s; s++)
      bodyLen += (*s == LINEFEED_CONTROL_CHAR) ? 2 : 1;

    if ((size_t)writePos + bodyLen + 1 > MAX_SHADER_TEXT_LEN) {
      wchar_t errMsg[512];
      swprintf(errMsg, 512,
               L"%hs shader too large to assemble: %zu bytes of preset text plus "
               L"%d bytes of engine prelude, limit %d",
               szWhichShader, bodyLen, writePos, (int)MAX_SHADER_TEXT_LEN);
      dumpmsg(errMsg, LOG_WARN);
      AddError(errMsg, 8.0f, ERR_PRESET, true);
      AutoFlagPresetError(PresetFileForCompileError(), errMsg);
      return false;
    }
  }

  {
    const char* s = szOrigShaderText;
    char* d = &szShaderText[writePos];
    while (*s) {
      if (*s == LINEFEED_CONTROL_CHAR) {
        *d++ = 13; writePos++;
        *d++ = 10; writePos++;
      }
      else {
        *d++ = *s; writePos++;
      }
      s++;
    }
    *d = 0; writePos++;
  }

  // MilkDrop3 mode markers ("//MilkDrop3 Color Mode:", "//MilkDrop3 Burn Mode:", etc.)
  // are just comments in the preset shader code. The actual operations (division by
  // negatives, lerp, etc.) follow on subsequent lines and execute correctly as-is.
  // Do NOT replace these markers with saturate() calls — doing so destroys color
  // information by clamping intermediate values before sign-flipping operations.

  // strip out all comments - but cheat a little - start at the shader test.
  // (the include file was already stripped of comments)
  StripComments(&szShaderText[shaderStartPos]);

  // Replace "i = I_MAX;" break hack with real "break;" — the hack was used by
  // Milkwave's transpiler for SM2.0 compatibility, but SM3.0+ supports break natively.
  // The hack can cause incorrect loop behavior with some D3DCompile optimization paths.
  {
    char* search = &szShaderText[shaderStartPos];
    while ((search = strstr(search, "i = I_MAX")) != NULL) {
      // Find the semicolon
      char* semi = strchr(search, ';');
      if (semi) {
        int len = (int)(semi - search + 1);
        memset(search, ' ', len);
        memcpy(search, "break;", 6);
        search = semi + 1;
      } else {
        break;
      }
    }
  }

  // Inject [loop] attribute before 'while' loops in preset shaders.
  // SM3.0 hardware couldn't unroll 100+ iteration loops; SM5.0 may attempt
  // partial unrolling which changes codegen and floating-point accumulation.
  // [loop] forces dynamic branching, producing results closer to SM3.0 behavior.
  // NOTE: Only 'while' loops — NOT 'for' loops. Small fixed-count for loops
  // (e.g. for(int i=0;i<3;i++)) cause error X3531 if marked [loop] because
  // the compiler insists on unrolling them. Raymarchers use while() loops.
  //
  // Output grows as needed (std::string), the same shape forgejo#18 and #99
  // used for the rewriters and _safe_denom below. This used to shuffle the
  // text along IN PLACE -- lstrcpyA the tail into `temp`, memcpy "[loop] " over
  // the 'w', then lstrcpyA the tail back 7 bytes later -- inside the fixed
  // MAX_SHADER_TEXT_LEN buffer, with nothing bounding it. Seven bytes per
  // `while`, so a body that FITS the buffer is still pushed off the end by a
  // few thousand loops. That is a second, independent route to forgejo#101's
  // silent death, and bounding the assembly above does not close it: measured
  // dead at 244419 bytes of preset text with 5200 while loops, which assembles
  // to 255246 and fits, then grows to 291646 and does not.
  {
    const char* base = &szShaderText[shaderStartPos];
    std::string out;
    out.reserve(strlen(base) + 1024);
    const char* p = base;
    while (*p) {
      bool isWhile = (p[0] == 'w' && p[1] == 'h' && p[2] == 'i' && p[3] == 'l' && p[4] == 'e' && (p[5] == ' ' || p[5] == '('));
      if (isWhile) {
        bool prevOk = (p == base) ||
                       (!isalnum((unsigned char)*(p - 1)) && *(p - 1) != '_');
        // Looks at what has already been EMITTED rather than at the source.
        // The in-place version could read back its own insertion; this one
        // cannot, and the emitted tail is the faithful equivalent -- it also
        // still catches a "[loop] while" the preset author wrote by hand.
        bool alreadyHasLoop =
            (out.size() >= 7 && out.compare(out.size() - 7, 7, "[loop] ") == 0) ||
            (out.size() >= 6 && out.compare(out.size() - 6, 6, "[loop]") == 0);
        if (prevOk && !alreadyHasLoop)
          out.append("[loop] ", 7);
      }
      out.push_back(*p++);
    }

    if ((size_t)shaderStartPos + out.size() + 1 > MAX_SHADER_TEXT_LEN) {
      wchar_t errMsg[512];
      swprintf(errMsg, 512,
               L"%hs shader too large after [loop] injection: %zu bytes, limit %d",
               szWhichShader, (size_t)shaderStartPos + out.size() + 1,
               (int)MAX_SHADER_TEXT_LEN);
      dumpmsg(errMsg, LOG_WARN);
      AddError(errMsg, 8.0f, ERR_PRESET, true);
      AutoFlagPresetError(PresetFileForCompileError(), errMsg);
      return false;
    }
    memcpy(&szShaderText[shaderStartPos], out.c_str(), out.size() + 1);
  }

  // Strip DX9-style "sampler sampler_randNN;" declarations from preset text.
  // The include already declares "Texture2D sampler_randNN;" — having both
  // causes a redefinition error (sampler = SamplerState in SM5.0).
  //
  // 0..3 ONLY, and that bound is load-bearing: the include declares exactly
  // sampler_rand00 through sampler_rand03 and no more. Raising it to 15 on the
  // assumption that the ceiling was arbitrary stripped the declarations that
  // presets using rand04 and above provide for THEMSELVES, and the next sweep
  // reported twelve new "undeclared identifier 'sampler_rand04'" errors.
  //
  // EVERY sampler keyword, not just the bare `sampler`. Presets overwhelmingly
  // write `sampler2D`, and matching only `sampler ` meant this strip almost
  // never fired -- which is why redefinitions of sampler_rand00 were the single
  // commonest compile failure in the corpus. Read out of the engine's own
  // generated-shader dump rather than guessed: line 460 was `sampler2D
  // sampler_pic;` against the include's `Texture2D sampler_rand00;` on 337.
  // TOKENISED, not matched literally. Three separate faults came out of
  // building the needle with sprintf and calling strstr:
  //
  //   "sampler X"   missed `sampler2D X`, which is how presets actually write
  //                 it -- so this strip almost never fired, and redefinition of
  //                 sampler_rand00 became the commonest failure in the corpus;
  //   one space     missed `sampler  sampler_rand03`, which is just as legal;
  //   prefix match  ate `sampler sampler_rand00_smalltiled` when stripping
  //                 sampler_rand00, leaving a needed declaration undeclared.
  //
  // So: keyword as a whole token, any run of spaces or tabs, the exact name
  // with a boundary after it, any spaces, a semicolon.
  // Remove ONE declarator named `name` from any sampler declaration, and blank
  // the whole statement when that was the only declarator.
  //
  // The declarator list is the fifth spelling of this declaration to turn up in
  // the corpus, after sampler2D, two spaces, the #define alias and the
  // state-block form (#305, #316, #318). Presets write
  //
  //     sampler sampler_rand00,sampler_rand01,sampler_rand02,sampler_rand03;
  //
  // and the previous version matched the keyword, matched sampler_rand00, then
  // required a ';' -- found a ',' and gave up, leaving the declaration to
  // collide with the include's own as "redefinition of 'sampler_rand00'".
  //
  // Blanking the whole statement is NOT the answer, and the second preset that
  // hit this shows why: it declares sampler_rand01 through sampler_rand15 in one
  // list, and the include provides only rand00..rand03. Dropping the statement
  // would take rand04..rand15 with it and turn a redefinition into fifteen
  // undeclared identifiers -- the same trap as raising the strip ceiling, which
  // was tried and reverted in #316. So the list is edited, not deleted.
  //
  // Everything is overwritten with spaces rather than spliced out, so byte
  // offsets and line numbers survive and the compiler's error lines still point
  // at the user's source.
  auto blankSamplerDecl = [](char* body, const char* name) {
    static const char* kw[] = {
        "sampler2D", "sampler1D", "sampler3D", "samplerCUBE", "sampler"
    };
    const size_t nameLen = strlen(name);
    auto isIdent = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    auto skipWs  = [](char* c) {
      while (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') c++;
      return c;
    };

    for (const char* k : kw) {
      const size_t kLen = strlen(k);
      for (char* p = strstr(body, k); p; p = strstr(p + 1, k)) {
        if (p != body && isIdent(p[-1]))
          continue;                                   // not a token start
        char* q = p + kLen;
        if (isIdent(*q))
          continue;                                   // "sampler" inside "sampler2D"
        q = skipWs(q);

        // Find where the statement ends. A state-block initialiser contains its
        // own semicolons, so brace depth decides. A '(' means this is a function
        // parameter or a call, not a declaration -- leave it alone, which is what
        // the old "next char must be ';'" test was really protecting against.
        char* end = q;
        int depth = 0;
        for (; *end; end++) {
          if (*end == '{') depth++;
          else if (*end == '}') { if (depth > 0) depth--; }
          else if (depth == 0 && (*end == '(' || *end == ')')) break;
          else if (depth == 0 && *end == ';') break;
        }
        if (*end != ';') continue;

        // Split the declarator list on top-level commas.
        struct Span { char* begin; char* end; };       // [begin, end)
        Span decl[64];
        int nDecl = 0;
        char* s = q;
        depth = 0;
        for (char* c = q; c <= end && nDecl < 64; c++) {
          if (*c == '{') depth++;
          else if (*c == '}') { if (depth > 0) depth--; }
          else if (depth == 0 && (*c == ',' || c == end)) {
            decl[nDecl].begin = s;
            decl[nDecl].end   = c;
            nDecl++;
            s = c + 1;
          }
        }
        if (nDecl == 0) continue;

        // Which declarator is this one? Its leading identifier is the name.
        int hit = -1;
        for (int i = 0; i < nDecl; i++) {
          char* a = skipWs(decl[i].begin);
          if (strncmp(a, name, nameLen) != 0) continue;
          if (isIdent(a[nameLen])) continue;           // a longer name
          hit = i;
          break;
        }
        if (hit < 0) continue;

        if (nDecl == 1) {
          memset(p, ' ', (size_t)(end - p) + 1);       // keyword through ';'
          continue;
        }

        // One of several: take the declarator and one adjacent comma with it, so
        // the remaining list stays well formed at either end.
        char* from = (hit == 0) ? decl[hit].begin : decl[hit - 1].end;
        char* to   = (hit == 0) ? decl[hit].end + 1 : decl[hit].end;  // past its comma
        if (to > end) to = end;
        memset(from, ' ', (size_t)(to - from));
      }
    }
  };

  for (int ri = 0; ri <= 3; ri++) {
    char name[32];
    sprintf(name, "sampler_rand%02d", ri);
    blankSamplerDecl(&szShaderText[shaderStartPos], name);
  }

  // Drop DX9 "= sampler_state { ... }" initialisers, keeping the declaration
  // itself (#318).
  //
  // The include maps the DX9 keyword straight through -- `#define sampler
  // Texture2D` -- which is what makes `sampler tex;` work at all. It also means
  //
  //     sampler sampler_cells = sampler_state { AddressU = mirror; };
  //
  // becomes `Texture2D sampler_cells = sampler_state {...};`, and the compiler
  // says exactly that:
  //
  //     error X3017: cannot implicitly convert from 'const StateBlock'
  //                  to 'Texture2D<float4>'
  //
  // The DECLARATION has to survive -- sampler_cells is a disk texture, and
  // reflection is what tells CacheParams to go and load `cells`. Blanking the
  // whole statement, as the sampler_rand strip above does, would leave it
  // undeclared instead. So only the initialiser goes.
  //
  // The ADDRESSING MODE IS KEPT. The block is parsed before it is removed, the
  // filter and address mode inside it are mapped to one of the shared
  // SamplerStates, and the sampler's tex2D calls are routed through that state
  // further down -- the same mechanism replaceTex2D already uses for the
  // built-in sampler_pw_*/sampler_fc_* names.
  //
  // Dropping the mode silently was the first version of this, and it is the
  // kind of fix that looks complete and changes the picture: a texture declared
  // `AddressU = mirror` sampled WRAP instead, with nothing said. MIRROR had
  // nowhere to go at all until _samp_lm/_samp_pm were added at s4/s5.
  std::vector<std::pair<std::string, std::string>> presetSamplerStates;
  {
    char* body = &szShaderText[shaderStartPos];
    for (char* s = strstr(body, "sampler_state"); s;
         s = strstr(s, "sampler_state")) {
      char* eq = s - 1;
      while (eq > body && (*eq == ' ' || *eq == '\t' || *eq == '\r' || *eq == '\n'))
        eq--;
      char* b = strchr(s, '{');
      if (eq <= body || *eq != '=' || !b) { s += 13; continue; }
      int depth = 0;
      char* c = b;
      for (; *c; c++) {
        if (*c == '{') depth++;
        else if (*c == '}') { if (--depth == 0) break; }
      }
      if (*c != '}') { s += 13; continue; }   // unbalanced; leave it alone

      // The name being declared, walking back from the '=' over whitespace.
      char* nameEnd = eq;
      while (nameEnd > body &&
             (nameEnd[-1] == ' ' || nameEnd[-1] == '\t' || nameEnd[-1] == '\r' ||
              nameEnd[-1] == '\n'))
        nameEnd--;
      char* nameStart = nameEnd;
      while (nameStart > body &&
             (isalnum((unsigned char)nameStart[-1]) || nameStart[-1] == '_'))
        nameStart--;
      const std::string name(nameStart, (size_t)(nameEnd - nameStart));

      // What the block asks for. Only the two axes the shared states cover;
      // anything else in there (MaxAnisotropy, MipFilter...) is ignored rather
      // than guessed at.
      std::string block(b, (size_t)(c - b) + 1);
      for (char& ch : block) ch = (char)tolower((unsigned char)ch);
      const bool wantPoint  = block.find("point") != std::string::npos;
      const bool wantMirror = block.find("mirror") != std::string::npos;
      const bool wantClamp  = block.find("clamp") != std::string::npos;
      const char* state = nullptr;
      if (wantMirror)     state = wantPoint ? "_samp_pm" : "_samp_lm";
      else if (wantClamp) state = wantPoint ? "_samp_pc" : "_samp_lc";
      else if (wantPoint) state = "_samp_pw";
      // plain LINEAR+WRAP is what tex2D already expands to; no rewrite needed.
      if (state && !name.empty())
        presetSamplerStates.emplace_back(name, state);

      memset(eq, ' ', (size_t)(c - eq) + 1);  // from '=' through '}', keeping ';'
      s = c + 1;
    }
  }

  // The same declaration reached through a #define ALIAS (#315).
  //
  // This is the single commonest failure in the whole preset corpus -- 156 of
  // roughly 180 shader errors across a 4,767-preset sweep, all of them
  // "redefinition of 'sampler_rand00'". Presets write:
  //
  //     #define sampler_pic sampler_rand00
  //     sampler sampler_pic;
  //
  // The compiler expands that to `sampler sampler_rand00;`, which collides with
  // the include's Texture2D of the same name. Neither strip above can see it:
  // both match literal text, and the alias is not resolved until compilation.
  // The strip's own comment describes exactly this pattern as the reason it
  // exists, and then matches on the one spelling that does not occur.
  //
  // So resolve the aliases first and strip what they expand to.
  {
    char* body = &szShaderText[shaderStartPos];
    for (const char* p = strstr(body, "#define "); p; p = strstr(p, "#define ")) {
      p += 8;
      while (*p == ' ' || *p == '\t') p++;
      const char* nameStart = p;
      while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
      const std::string alias(nameStart, p - nameStart);
      const char* q = p;
      while (*q == ' ' || *q == '\t') q++;
      if (alias.empty() || strncmp(q, "sampler_rand", 12) != 0)
        continue;

      // Same tokeniser as the direct case above, so the alias path cannot
      // drift away from it -- and it is the alias path that matters most here,
      // because `#define sampler_pic sampler_rand00` + `sampler2D sampler_pic;`
      // is by far the commonest way a preset produces the duplicate.
      blankSamplerDecl(body, alias.c_str());
    }
  }

  // Shader inputs/outputs (injected automatically, not visible in preset code):
  //
  // WARP shader:
  //   Inputs:  float2 uv       - current texture coordinate
  //            float2 uv_orig  - original (unwarped) texture coordinate
  //            float  rad      - distance from center (0..~0.7)
  //            float  ang      - angle from center (radians)
  //   Samplers: sampler_main (t0), sampler_blur1..blur6, sampler_* (disk textures)
  //   Output:  float3 ret      - warped UV (ret.xy = new uv, ret.z unused)
  //
  // COMP (composite) shader:
  //   Inputs:  float2 uv       - screen texture coordinate
  //            float  rad      - distance from center
  //            float  ang      - angle from center
  //            float3 hue_shader - preset hue color (from per-frame equations)
  //   Samplers: sampler_main (t0), sampler_blur1..blur6, sampler_* (disk textures)
  //   Output:  float3 ret      - final RGB color

  /*
  1. paste warp or comp #defines
  2. search for "void" + whitespace + szFn + [whitespace] + '('
  3. insert params
  4. search for [whitespace] + ')'.
  5. search for final '}' (strrchr)
  6. back up one char, insert the Last Line, and add '}' and that's it.
  */
  if ((shaderType == SHADER_WARP || shaderType == SHADER_COMP) && szProfile[0] == 'p') {
    char* p = &szShaderText[shaderStartPos];

    // seek to 'shader_body' and replace it with spaces
    while (*p && strncmp(p, "shader_body", 11))
      p++;

    // DIAG: log whether shader_body was found
    DLOG_VERBOSE("DIAG shader_body search: type=%d found=%d offsetFromStart=%d",
              shaderType, (*p != 0) ? 1 : 0, (int)(p - &szShaderText[shaderStartPos]));

    if (*p) {
      for (int i = 0; i < 11; i++)
        *p++ = ' ';
    }
    else {
      p = NULL; // shader_body not found — signal error
    }

    if (p) {
      // insert "void PS(...params...)\n"
      //
      // The third of the in-place growth steps that shared this fixed buffer
      // with nothing bounding them (forgejo#101). This one grows by a constant
      // -- 134 bytes for the current parameter list -- exactly once, so its
      // window is narrow rather than absent: an assembled text within 134 bytes
      // of MAX_SHADER_TEXT_LEN passes both bounds above and still runs off the
      // end here. Measured dead at a 251300-byte body, which assembles to
      // 262127 and fits, then reaches 262261 and does not.
      const char* params = (shaderType == SHADER_WARP) ? szWarpParams : szCompParams;
      char psLine[512];
      int psLen = FormatToA(psLine, "void %s( %s )\n", szFn, params);
      size_t curLen = strlen(szShaderText);
      if (psLen < 0 || curLen + (size_t)psLen + 1 > MAX_SHADER_TEXT_LEN) {
        wchar_t errMsg[512];
        swprintf(errMsg, 512,
                 L"%hs shader too large after entry-point injection: %zu bytes, limit %d",
                 szWhichShader, curLen + (psLen < 0 ? 0 : (size_t)psLen) + 1,
                 (int)MAX_SHADER_TEXT_LEN);
        dumpmsg(errMsg, LOG_WARN);
        AddError(errMsg, 8.0f, ERR_PRESET, true);
        AutoFlagPresetError(PresetFileForCompileError(), errMsg);
        return false;
      }
      strncpy_s(temp, MAX_SHADER_TEXT_LEN, p, _TRUNCATE);
      memcpy(p, psLine, (size_t)psLen);
      p[psLen] = 0;
      // Injections run forward through the buffer, so counting newlines up to
      // `p` right now gives this line's number in the FINAL text -- nothing
      // inserted later can sit before it.
      m_shaderInjectedLines.push_back(LineOfOffset(szShaderText, (size_t)(p - szShaderText)));
      p += psLen;
      strncpy_s(p, MAX_SHADER_TEXT_LEN - (size_t)(p - szShaderText), temp, _TRUNCATE);

      // find the starting curly brace
      p = strchr(p, '{');
      if (p) {
        // skip over it
        p++;
        // then insert first line(s)
        std::string firstLine;
        if (m_bLoadingShadertoyMode && !bHardErrors) {
          // Shadertoy: float4 ret to preserve alpha channel (temporal accumulation data)
          // Use m_bLoadingShadertoyMode (set before async thread) not m_bShadertoyMode
          // (set after compilation in LoadPresetTick — too late for shader text generation)
          firstLine = "    float4 ret = 0;\n";
        } else {
          firstLine = std::string(szFirstLine) + "\n";
        }
        const int injFirst = LineOfOffset(szShaderText, (size_t)(p - szShaderText));
        if (!insertAt(p, firstLine.c_str(), "first-line injection")) return false;
        m_shaderInjectedLines.push_back(injFirst);

        // Compute rad/ang per-pixel from UV for ALL comp/buffer shaders.
        // Fullscreen TRIANGLESTRIP quads hardcode rad=1 and ang=0/π on left/right
        // verts — linear interpolation across the strip diagonal creates a visible
        // "triangle" seam (and can NaN polar math). Shadertoy used to skip this
        // injection; that was wrong for any preset using rad/ang (or macros).
        // _c0.xy = (aspect_x, aspect_y), UV in [0,1] → NDC in [-1,1].
        // atan2 is redirected to _safe_atan2 via include.fx macros.
        if (shaderType == SHADER_COMP) {
          // Four injected lines, so four entries -- the map counts lines, not
          // insertion sites. Computed before the insertion; the text ahead of
          // p does not move, so the value is the same either way.
          const int injLine = LineOfOffset(szShaderText, (size_t)(p - szShaderText));
          if (!insertAt(p,
                "    {\n"
                "      float2 __d = float2((_uv.x * 2.0 - 1.0) * _c0.x, (_uv.y * 2.0 - 1.0) * _c0.y);\n"
                "      _rad_ang = float2(length(__d), atan2(__d.y, __d.x));\n"
                "    }\n",
                "rad/ang injection"))
            return false;
          for (int k = 0; k < 4; k++)
            m_shaderInjectedLines.push_back(injLine + k);
        }

        // find the ending curly brace
        p = strrchr(p, '}');
        if (p) {
          if (m_bLoadingShadertoyMode && !bHardErrors) {
            // Shadertoy output: Buffer A/B preserve full float4 (alpha stores data);
            // Image/comp forces alpha=1 (shaders that write .rgb leave alpha=0 which
            // would be transparent — the old non-Shadertoy wrapper used _vDiffuse.w=1).
            // Always strip NaNs via _finite* so one bad pixel cannot poison feedback.
            bool bIsBuffer = szDiagName && (strcmp(szDiagName, "bufferA") == 0 || strcmp(szDiagName, "bufferB") == 0 ||
                                            strcmp(szDiagName, "bufferC") == 0 || strcmp(szDiagName, "bufferD") == 0);
            const char* szLastLine = bIsBuffer
              ? "    _return_value = _finite4(ret);"
              : "    _return_value = float4(_finite3(ret.xyz), 1.0);";
            char tail[256];
            FormatToA(tail, " %s\n}\n", szLastLine);
            if (!writeTailAt(p, tail, "shader-output injection")) return false;
          } else {
            // MilkDrop3 does NOT apply gamma_adj or B/D/S/I for custom comp shader presets.
            // gamma_adj is only used in ShowToUser_NoShaders path (no custom comp shader).
            // shiftHSV is an MDropDX12 addition for colshift; early-exits when colshift values are 0.
            //
            // Warp decay: DX9 applied decay via fixed-function texture stage modulate
            // (D3DTSS_COLOROP = D3DTOP_MODULATE, D3DTSS_COLORARG1 = D3DTA_DIFFUSE) which
            // multiplied shader output by vertex color. In DX12, we inject this into the
            // shader body. Auto-gen warp (GenWarpPShaderText) already includes decay;
            // custom warp shaders do not — they relied on the fixed-function multiply.
            // _finite3 strips NaNs so warp UV / color feedback cannot wedge the frame.
            if (shaderType == SHADER_WARP) {
              if (!writeTailAt(p,
                    "    ret.xyz *= _vDiffuse.rgb;\n"
                    "    _return_value = float4(shiftHSV(_finite3(ret.xyz)), _vDiffuse.w);\n}\n",
                    "shader-output injection"))
                return false;
            } else {
              if (!writeTailAt(p,
                    "     _return_value = float4(shiftHSV(_finite3(ret.xyz)), _vDiffuse.w);\n}\n",
                    "shader-output injection"))
                return false;
            }
          }
        }
      }
    }

    if (!p) {
      wchar_t errMsg[512];
      FormatResTo(errMsg, IDS_ERROR_PARSING_X_X_SHADER, szProfile, szWhichShader);
      dumpmsg(errMsg, LOG_WARN);
      AddError(errMsg, 8.0f, ERR_PRESET, true);
      AutoFlagPresetError(PresetFileForCompileError(), errMsg);
      return false;
    }
  }

  // ---- Source rewrites, all of which may lengthen the text ----------------
  //
  // These run on a std::string rather than on szShaderText directly. Each used
  // to write into a malloc'd buffer sized ONCE as (srcLen + 32768) above a loop
  // that lengthens the text on every pass -- 28 passes for FixShadowedBuiltins,
  // up to 64 for FixMatrixVarMultiply -- and then memcpy'd the result back into
  // this fixed MAX_SHADER_TEXT_LEN buffer with no bound at either end. Both
  // overruns are driven purely by the contents of a preset file, and this app
  // registers .milk/.milk2 file associations and accepts drag-and-drop, so the
  // input is "a preset someone sent you". See forgejo#18.
  //
  // There is now exactly one write back into the fixed buffer, and it is
  // checked.
  {
    std::string src(szShaderText);

    // Zero-initialize bare locals BEFORE the renaming passes, so it sees
    // declarations in the form the preset author wrote them. Adding "= 0"
    // cannot affect what the passes below rename.
    FixUninitializedLocals(src);

    // Fix variables that shadow HLSL built-in functions (e.g. float2 pow = ...)
    FixShadowedBuiltins(src);

    // Fix local rad/ang that collide with MilkDrop's #define rad/ang → _rad_ang.x/.y
    // (causes X3036 redefinition of formal parameter _rad_ang and failed preset switches)
    FixMilkdropRadAngShadowing(src);

    // Must see the ORIGINAL `aspect`, and it renames uses after the
    // declaration only, so it is independent of the passes around it.
    FixSelfReferentialMacroShadow(src);

    // Fix variables that shadow user-defined functions (e.g. float2 R2D = mul(uv, R2D(...)))
    FixShadowedUserFunctions(src);

    // Fix self-referencing variable redeclarations (e.g. float3 ret1 = ret1;)
    FixSelfRedeclarations(src);

    // Fix matrix * vector multiplication (HLSL requires mul())
    FixMatrixVarMultiply(src);

    // FixSelfRedeclarations blanks a type keyword with spaces in place, so the
    // string can carry a NUL before its end; trim to the C string the rest of
    // this function expects.
    src.resize(strlen(src.c_str()));

    // LAST, deliberately (#326). It inserts a keyword ahead of a type, and
    // every pass above matches on `<type> <name>` -- running it earlier would
    // put `static ` between them for no benefit, since nothing above cares
    // whether a global is static. Scoped to the preset's own text: the prelude
    // before shaderStartPos declares the real uniforms.
    MakePresetGlobalsStatic(src, (size_t)shaderStartPos);

    if (src.size() + 1 > MAX_SHADER_TEXT_LEN) {
      wchar_t errMsg[512];
      swprintf(errMsg, 512,
               L"%hs shader too large after preprocessing: %zu bytes, limit %d",
               szWhichShader, src.size() + 1, (int)MAX_SHADER_TEXT_LEN);
      dumpmsg(errMsg, LOG_WARN);
      AddError(errMsg, 8.0f, ERR_PRESET, true);
      AutoFlagPresetError(PresetFileForCompileError(), errMsg);
      return false;
    }
    memcpy(szShaderText, src.c_str(), src.size() + 1);
  }

  // Safe division: wrap variable denominators with _safe_denom() inside the main PS body only.
  // DX9 returns 0 for 0/0 (non-IEEE); DX12 returns NaN (IEEE 754) which propagates through
  // += and poisons the entire frame. _safe_denom(0)=1e-30, so 0/1e-30=0 matching DX9.
  // IMPORTANT: Only process inside void PS(...){...} — NOT helper functions before shader_body.
  // Output grows as needed (std::string, forgejo#99) instead of a malloc(totalLen + 16384)
  // whose write loop had no bound check until AFTER the loop completed -- a per-pixel body
  // with enough bare denominators to expand past that fixed 16KB headroom overflowed the
  // heap allocation while still writing, the same bug class #18 fixed for the six rewriters
  // above (this pass runs after them and was not touched by that fix).
  if ((shaderType == SHADER_WARP || shaderType == SHADER_COMP) && szProfile[0] == 'p') {
    char* psStart = strstr(szShaderText, "void PS(");
    if (psStart) {
      char* bodyStart = strchr(psStart, '{');
      if (bodyStart) {
        bodyStart++;
        int depth = 1;
        char* bodyEnd = bodyStart;
        while (*bodyEnd && depth > 0) {
          if (*bodyEnd == '{') depth++;
          else if (*bodyEnd == '}') depth--;
          if (depth > 0) bodyEnd++;
        }
        std::string out;
        out.reserve(strlen(szShaderText) + 1024);
        // Copy everything before bodyStart verbatim
        size_t prefixLen = bodyStart - szShaderText;
        out.append(szShaderText, prefixLen);
        char* src = bodyStart;
        auto isIdent = [](char c) -> bool {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };
        while (src < bodyEnd) {
          if (*src == '/' && src[1] != '/' && src[1] != '*' && src[1] != '=') {
            if (src > szShaderText && *(src - 1) == '*') { out.push_back(*src++); continue; }
            out.push_back(*src++); // copy '/'
            // Skip whitespace (copy to output)
            while (*src == ' ' || *src == '\t') out.push_back(*src++);
            // Skip numeric/dot constants
            if ((*src >= '0' && *src <= '9') || *src == '.') continue;
            if (*src == '-') {
              char* am = src + 1; while (*am == ' ') am++;
              if (*am >= '0' && *am <= '9') continue;
            }
            if (*src == '(') {
              // Parenthesized expr — find matching ')' and wrap.
              // Also handle HLSL C-style casts: (float)EJECTA, (float2)v, (int)(x+1).
              // Without this, matching stops at the cast's ')' and emits
              // _safe_denom((float)) EJECTA which fails to compile.
              out.append("_safe_denom(", 12);
              int pd = 1;
              char* parenOpen = src;
              out.push_back(*src++); // copy '('
              while (*src && pd > 0) {
                if (*src == '(') pd++;
                else if (*src == ')') pd--;
                out.push_back(*src++);
              }
              // Detect cast: single type token inside parens + operand after
              bool looksLikeCast = false;
              {
                char* t = parenOpen + 1;
                while (*t == ' ' || *t == '\t') t++;
                char* t0 = t;
                while (isIdent(*t)) t++;
                char* t1 = t;
                while (*t == ' ' || *t == '\t') t++;
                if (*t == ')' && t1 > t0) {
                  // Common HLSL cast type names (and any single identifier)
                  looksLikeCast = true;
                }
              }
              if (looksLikeCast) {
                while (*src == ' ' || *src == '\t') out.push_back(*src++);
                if (isIdent(*src) && !(*src >= '0' && *src <= '9')) {
                  while (isIdent(*src)) out.push_back(*src++);
                  while (*src == '.') { out.push_back(*src++); while (isIdent(*src)) out.push_back(*src++); }
                } else if (*src == '(') {
                  int pd2 = 1;
                  out.push_back(*src++);
                  while (*src && pd2 > 0) {
                    if (*src == '(') pd2++;
                    else if (*src == ')') pd2--;
                    out.push_back(*src++);
                  }
                }
              }
              out.push_back(')'); // close _safe_denom
              continue;
            }
            if (isIdent(*src) && !(*src >= '0' && *src <= '9')) {
              char* idStart = src;
              while (isIdent(*src)) src++;
              // Check for function call
              char* peek = src;
              while (*peek == ' ' || *peek == '\t') peek++;
              if (*peek == '(') {
                // Function call — copy name + args, wrap whole thing
                out.append("_safe_denom(", 12);
                // Copy identifier
                out.append(idStart, src - idStart);
                // Copy whitespace between name and (
                while (src < peek) out.push_back(*src++);
                // Copy parens + args
                int pd = 1;
                out.push_back(*src++); // copy '('
                while (*src && pd > 0) {
                  if (*src == '(') pd++;
                  else if (*src == ')') pd--;
                  out.push_back(*src++);
                }
                out.push_back(')'); // close _safe_denom
              } else {
                // Simple variable (+ .swizzle)
                out.append("_safe_denom(", 12);
                out.append(idStart, src - idStart);
                while (*src == '.') { out.push_back(*src++); while (isIdent(*src)) out.push_back(*src++); }
                out.push_back(')'); // close _safe_denom
              }
              continue;
            }
            continue;
          }
          out.push_back(*src++);
        }
        // Copy from bodyEnd to end of string (closing '}' + any trailing text)
        out.append(bodyEnd);

        if (out.size() + 1 > MAX_SHADER_TEXT_LEN) {
          wchar_t errMsg[512];
          swprintf(errMsg, 512,
                   L"%hs shader too large after _safe_denom wrapping: %zu bytes, limit %d",
                   szWhichShader, out.size() + 1, (int)MAX_SHADER_TEXT_LEN);
          dumpmsg(errMsg, LOG_WARN);
          AddError(errMsg, 8.0f, ERR_PRESET, true);
          AutoFlagPresetError(PresetFileForCompileError(), errMsg);
          return false;
        }
        memcpy(szShaderText, out.c_str(), out.size() + 1);
      }
    }
  }

  // Collapse whitespace between normalize and '(' — e.g. "normalize (" → "normalize("
  // Some presets have spaces here which would skip the _safe_normalize replacement.
  {
    const char* fn = "normalize";
    int fnLen = (int)strlen(fn);
    char* p = szShaderText;
    while ((p = strstr(p, fn)) != nullptr) {
      if (p > szShaderText && ((*(p-1) >= 'a' && *(p-1) <= 'z') || (*(p-1) >= 'A' && *(p-1) <= 'Z') || (*(p-1) >= '0' && *(p-1) <= '9') || *(p-1) == '_')) {
        p += fnLen; continue;
      }
      char* after = p + fnLen;
      int spaces = 0;
      while (after[spaces] == ' ' || after[spaces] == '\t') spaces++;
      if (spaces > 0 && after[spaces] == '(') {
        memmove(after, after + spaces, strlen(after + spaces) + 1);
      }
      p = after + 1;
    }
  }

  // Replace normalize() with _safe_normalize() to prevent NaN from zero-length vectors.
  // DX9 SM3.0 normalize(0) returns 0; DX12 SM5.0 returns NaN which propagates through
  // the feedback loop and darkens the image over time.
  //
  // Output grows as needed (std::string), then one bounded write-back. This
  // used to memmove the tail SIX BYTES further out per occurrence, in the fixed
  // buffer, under a comment that said "Expand buffer to fit the longer
  // replacement" -- nothing expanded, and nothing checked. Six bytes each is
  // open-ended, so unlike the constant-growth injections this needs no shader
  // near the cap to reach: measured dead at 216102 bytes of preset text with
  // 6000 normalize() calls, which assembles to 232933 and fits comfortably,
  // then grows to 268933 and does not. forgejo#101.
  {
    const char* src = "normalize(";
    const char* dst = "_safe_normalize(";
    const size_t srcLen = strlen(src);
    const size_t dstLen = strlen(dst);
    std::string out;
    out.reserve(strlen(szShaderText) + 1024);
    const char* p = szShaderText;
    const char* hit;
    while ((hit = strstr(p, src)) != nullptr) {
      // Word boundary check: skip if preceded by alphanumeric or underscore
      bool boundaryOk = true;
      if (hit > szShaderText) {
        char prev = *(hit - 1);
        if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9') || prev == '_')
          boundaryOk = false;
      }
      out.append(p, hit - p);
      out.append(boundaryOk ? dst : src, boundaryOk ? dstLen : srcLen);
      p = hit + srcLen;
    }
    if (p != szShaderText) {           // only rewrite if something matched
      out.append(p);
      if (out.size() + 1 > MAX_SHADER_TEXT_LEN)
        return tooLarge("normalize() rewrite", out.size() + 1);
      memcpy(szShaderText, out.c_str(), out.size() + 1);
    }
  }

  // Replace tex2D/tex2Dlod calls for samplers that need non-default addressing modes.
  // The generic tex2D macro uses _samp_lw (LINEAR+WRAP). These replacements bypass
  // the macro by directly emitting .Sample() / .SampleLevel() with the correct sampler.
  //
  // First: normalize whitespace between tex2D/tex2Dlod/tex2Dbias and '(' so that
  // "tex2D (sampler_pc_main, ...)" matches the replacement pattern.
  // Some presets have spaces here (e.g. organic12-3d-2.milk), which would otherwise
  // skip the replacement and fall through to the tex2D macro with the wrong sampler.
  {
    const char* texFuncs[] = { "tex2Dlod", "tex2Dbias", "tex2D", "tex3Dlod", "tex3D" };
    for (const char* fn : texFuncs) {
      int fnLen = (int)strlen(fn);
      char* p = szShaderText;
      while ((p = strstr(p, fn)) != nullptr) {
        // Ensure we're at a word boundary (not inside another identifier)
        if (p > szShaderText) {
          char prev = *(p - 1);
          if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') || (prev >= '0' && prev <= '9') || prev == '_') {
            p += fnLen;
            continue;
          }
        }
        char* after = p + fnLen;
        // Count whitespace between function name and '('
        int spaces = 0;
        while (after[spaces] == ' ' || after[spaces] == '\t') spaces++;
        if (spaces > 0 && after[spaces] == '(') {
          // Collapse: shift everything left by 'spaces' chars
          memmove(after, after + spaces, strlen(after + spaces) + 1);
        }
        p = after + 1;  // advance past the '('
      }
    }
    // Also strip whitespace after '(' — e.g. "tex2D( sampler_pw_main," → "tex2D(sampler_pw_main,"
    // Without this, replaceTex2D won't match and the call falls through to the tex2D macro
    // which uses _samp_lw (LINEAR+WRAP) instead of the correct sampler for pw_/fc_/pc_ prefixes.
    // A macro call whose '(' is on the NEXT line.
    //
    // MilkDrop stores shader code as numbered lines and authors break them
    // wherever they like, including right after the macro name:
    //
    //     comp_20=`... max(GetBlur1(bg_uv).z*1.5,tex2D
    //     comp_21=`( sampler_main, mid_uv).z));
    //
    // Standard C preprocessing expands a function-like macro whose argument
    // list starts on a later line. fxc does NOT -- verified with a two-line
    // repro: identical source compiles with the call on one line and fails
    // with the '(' moved down. `tex2D` then falls through to the legacy
    // tex2D(sampler2D, float2) intrinsic, which has no Texture2D overload:
    //
    //     error X3013: 'tex2D': no matching 2 parameter intrinsic function
    //
    // 13 of 633 presets in one folder hit this, and nothing reported it: the
    // engine logs FAIL RecompilePShader, falls back, and records the preset as
    // applied.
    //
    // The name is moved DOWN to meet its '(' rather than the '(' pulled up,
    // because that rotates the same characters within the same span and so
    // leaves every line number unchanged -- the Preset Editor maps compile
    // errors back to the user's own source line, and joining lines here would
    // silently shift every mapping after this point.
    {
      static const char* const kFuncMacros[] = {
        "tex2Dlod", "tex2dlod", "tex2Dbias", "tex3Dlod", "tex2D", "tex2d",
        "tex3D", "tex3d", "GetBlur1", "GetBlur2", "GetBlur3", "GetMain",
        "GetPixel", "lum",
      };
      for (const char* name : kFuncMacros) {
        const int nameLen = (int)strlen(name);
        char* p = szShaderText;
        while ((p = strstr(p, name)) != nullptr) {
          // Whole identifier only: `mytex2D` must not match `tex2D`.
          const bool atWordStart =
              (p == szShaderText) ||
              !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
          char* q = p + nameLen;
          if (!atWordStart || isalnum((unsigned char)*q) || *q == '_') {
            p += nameLen;
            continue;
          }
          // Must be followed by whitespace containing at least one newline,
          // then '(' -- anything else is either a normal call or not a call.
          char* s = q;
          bool sawNewline = false;
          while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
            if (*s == '\n') sawNewline = true;
            s++;
          }
          if (!sawNewline || *s != '(') {
            p += nameLen;
            continue;
          }
          // Rotate [name][gap] into [gap][name]; same characters, same length,
          // same number of newlines, so line numbers are untouched.
          const size_t gapLen = (size_t)(s - q);
          memmove(p, q, gapLen);
          memcpy(p + gapLen, name, nameLen);
          p = s;   // now points at '('
        }
      }
    }

    {
      const char* texFuncs2[] = { "tex2Dlod(", "tex2Dbias(", "tex2D(", "tex3Dlod(", "tex3D(" };
      for (const char* fn : texFuncs2) {
        int fnLen = (int)strlen(fn);
        char* p = szShaderText;
        while ((p = strstr(p, fn)) != nullptr) {
          char* after = p + fnLen;  // points to char right after '('
          int spaces = 0;
          while (after[spaces] == ' ' || after[spaces] == '\t') spaces++;
          if (spaces > 0) {
            memmove(after, after + spaces, strlen(after + spaces) + 1);
          }
          p = after;
        }
      }
    }
    // Helper: find-and-replace all occurrences of `from` with `to` in szShaderText
    //
    // Output grows as needed, then one bounded write-back. Every tex2D
    // redirection below is longer than what it replaces
    // ("tex2D(sampler_x," -> "sampler_x.Sample(_samp_xx,"), and this used to
    // memmove the tail further out once per occurrence inside the fixed buffer
    // with nothing checking it -- the same open-ended growth as the normalize()
    // rewrite above. forgejo#101.
    //
    // Sets bReplaceOverflow rather than returning, because it is called a dozen
    // times below from a context that cannot propagate a failure cleanly; the
    // flag is checked once when they are all done.
    bool bReplaceOverflow = false;
    size_t nReplaceNeeded = 0;
    auto replacePattern = [&](const char* from, const char* to) {
      if (bReplaceOverflow) return;
      const size_t fromLen = strlen(from);
      const size_t toLen = strlen(to);
      const char* p = szShaderText;
      const char* hit = strstr(p, from);
      if (!hit) return;                // nothing matched: no copy, no rewrite
      std::string out;
      out.reserve(strlen(szShaderText) + 1024);
      while (hit) {
        out.append(p, hit - p);
        out.append(to, toLen);
        p = hit + fromLen;             // does not rescan the replacement,
        hit = strstr(p, from);         // matching the p += toLen it replaces
      }
      out.append(p);
      if (out.size() + 1 > MAX_SHADER_TEXT_LEN) {
        bReplaceOverflow = true;
        nReplaceNeeded = out.size() + 1;
        return;
      }
      memcpy(szShaderText, out.c_str(), out.size() + 1);
    };
    auto replaceTex2D = [&](const char* sampName, const char* sampState) {
      char from[80], to[80];
      // tex2D(sampler_name, ...) and tex2d(sampler_name, ...) → sampler_name.Sample(sampState, ...)
      // Both uppercase and lowercase variants must be handled — presets may use either.
      // The macros in embedded_shaders.h expand both to _samp_lw (WRAP); this text-replace
      // intercepts before macro expansion to redirect special samplers to the correct state.
      sprintf(to, "%s.Sample(%s,", sampName, sampState);
      sprintf(from, "tex2D(%s,", sampName);
      replacePattern(from, to);
      sprintf(from, "tex2d(%s,", sampName);
      replacePattern(from, to);
      // tex2Dlod / tex2dlod → sampler_name.SampleLevel(sampState, ...)
      sprintf(to, "%s.SampleLevel(%s,", sampName, sampState);
      sprintf(from, "tex2Dlod(%s,", sampName);
      replacePattern(from, to);
      sprintf(from, "tex2dlod(%s,", sampName);
      replacePattern(from, to);
    };
    // The preset's OWN sampler_state declarations first (#318), so a preset
    // that names a sampler after a built-in still gets what it actually asked
    // for rather than the built-in's assumption. replacePattern only rewrites
    // what is still spelled tex2D(name, so the later built-in calls cannot undo
    // one of these.
    for (const auto& ps : presetSamplerStates)
      replaceTex2D(ps.first.c_str(), ps.second.c_str());

    replaceTex2D("sampler_fc_main", "_samp_lc");  // LINEAR+CLAMP
    replaceTex2D("sampler_pc_main", "_samp_pc");  // POINT+CLAMP
    replaceTex2D("sampler_pw_main", "_samp_pw");  // POINT+WRAP
    // Noise/volume/random textures with non-default addressing modes.
    // Presets use sampler_pw_*, sampler_fc_*, sampler_pc_* prefixes to override
    // the default LINEAR+WRAP filtering/addressing. Without these replacements,
    // tex2D() falls through to the _samp_lw macro (LINEAR+WRAP), causing e.g.
    // point-filtered noise to appear smoothed/blurred ("glow" artifacts).
    replaceTex2D("sampler_pw_noise_lq",      "_samp_pw");  // POINT+WRAP
    replaceTex2D("sampler_pw_noise_lq_lite", "_samp_pw");
    replaceTex2D("sampler_pw_noise_mq",      "_samp_pw");
    replaceTex2D("sampler_pw_noise_hq",      "_samp_pw");
    replaceTex2D("sampler_pw_noisevol_lq",   "_samp_pw");
    replaceTex2D("sampler_pw_noisevol_hq",   "_samp_pw");
    replaceTex2D("sampler_fc_noise_lq",      "_samp_lc");  // LINEAR+CLAMP
    replaceTex2D("sampler_fc_noise_lq_lite", "_samp_lc");
    replaceTex2D("sampler_fc_noise_mq",      "_samp_lc");
    replaceTex2D("sampler_fc_noise_hq",      "_samp_lc");
    replaceTex2D("sampler_fc_noisevol_lq",   "_samp_lc");
    replaceTex2D("sampler_fc_noisevol_hq",   "_samp_lc");
    replaceTex2D("sampler_pc_noise_lq",      "_samp_pc");  // POINT+CLAMP
    replaceTex2D("sampler_pc_noise_lq_lite", "_samp_pc");
    replaceTex2D("sampler_pc_noise_mq",      "_samp_pc");
    replaceTex2D("sampler_pc_noise_hq",      "_samp_pc");
    replaceTex2D("sampler_pc_noisevol_lq",   "_samp_pc");
    replaceTex2D("sampler_pc_noisevol_hq",   "_samp_pc");
    // Random textures (rand00..rand03) with prefixed addressing
    replaceTex2D("sampler_pw_rand00",  "_samp_pw");
    replaceTex2D("sampler_pw_rand01",  "_samp_pw");
    replaceTex2D("sampler_pw_rand02",  "_samp_pw");
    replaceTex2D("sampler_pw_rand03",  "_samp_pw");
    replaceTex2D("sampler_fc_rand00",  "_samp_lc");
    replaceTex2D("sampler_fc_rand01",  "_samp_lc");
    replaceTex2D("sampler_fc_rand02",  "_samp_lc");
    replaceTex2D("sampler_fc_rand03",  "_samp_lc");
    replaceTex2D("sampler_pc_rand00",  "_samp_pc");
    replaceTex2D("sampler_pc_rand01",  "_samp_pc");
    replaceTex2D("sampler_pc_rand02",  "_samp_pc");
    replaceTex2D("sampler_pc_rand03",  "_samp_pc");
    replaceTex2D("sampler_blur1",   "_samp_lc");  // blur = LINEAR+CLAMP
    replaceTex2D("sampler_blur2",   "_samp_lc");
    replaceTex2D("sampler_blur3",   "_samp_lc");
    // Blur shaders: sampler_main needs CLAMP addressing
    if (shaderType == SHADER_BLUR) {
      replaceTex2D("sampler_main", "_samp_lc");
    }

    // Checked once, here, rather than at each of the ~90 replacePattern calls
    // above. The first one to overflow leaves the text untouched and latches
    // the flag, so nothing below runs on a half-rewritten shader.
    if (bReplaceOverflow)
      return tooLarge("tex2D sampler rewrite", nReplaceNeeded);
  }

  // Dump assembled shader text to file for diagnostics (Verbose only)
  if (DLOG_DIAG_ENABLED() && (shaderType == SHADER_COMP || shaderType == SHADER_WARP)) {
    const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
    wchar_t diagName[64];
    FormatTo(diagName, L"diag_%hs_shader.txt", typeName);
    FILE* f = DebugLogDiagOpen(diagName, L"w");
    if (f) {
      fprintf(f, "// DIAG: type=%s profile=%s len=%d preset=%ls\n",
              typeName, szProfile, lstrlenA(szShaderText),
              m_pState ? m_pState->m_szDesc : L"(unknown)");
      fputs(szShaderText, f);
      fclose(f);
    }
  }

  // now really try to compile it.

  bool failed = false;
  int len = lstrlenA(szShaderText);

  uint32_t checksum = crc32(szShaderText, len);

  DLOG_VERBOSE("DX12: LoadShaderFromMemory: checksum=0x%08X caching=%d", checksum, m_ShaderCaching);

  if (m_ShaderCaching) {
    pShaderByteCode = LoadShaderBytecodeFromFile(checksum, &szProfile[0]);
    DLOG_VERBOSE("DX12: LoadShaderFromMemory: cache %s (bytecode=%p)", pShaderByteCode ? "HIT" : "MISS", (void*)pShaderByteCode);
  }

  if (pShaderByteCode != NULL && !compileOnly) {
    DebugLogA("DX12: LoadShaderFromMemory: using cached bytecode, creating CT via D3DReflect...", LOG_VERBOSE);
    // restore ConstTable from cached bytecode via D3DReflect
    *ppConstTable = DX12ConstantTable::CreateFromBytecode(
      pShaderByteCode->GetBufferPointer(),
      pShaderByteCode->GetBufferSize());
    if (!*ppConstTable) {
      // Stale cache: old SM3.0 bytecode that D3DReflect can't parse.
      // Discard and fall through to recompile as SM5.0.
      DebugLogA("DX12: LoadShaderFromMemory: stale cache (D3DReflect failed), recompiling...", LOG_VERBOSE);
      pShaderByteCode->Release();
      pShaderByteCode = NULL;
    } else {
      DebugLogA("DX12: LoadShaderFromMemory: CT from cache done", LOG_VERBOSE);
    }
  }
  if (pShaderByteCode == NULL) {
    DebugLogA("DX12: LoadShaderFromMemory: compiling shader with D3DCompile...", LOG_VERBOSE);
    LARGE_INTEGER compileStart, compileEnd;
    LONGLONG compileFreq = GetCachedQPF();
    QueryPerformanceCounter(&compileStart);

    HRESULT hresult = D3DXCompileShader(
      szShaderText,
      len,
      NULL,//CONST D3DXMACRO* pDefines,
      NULL,//LPD3DXINCLUDE pInclude,
      szFn,
      szProfile,
      m_dwShaderFlags,
      &pShaderByteCode,
      &m_pShaderCompileErrors,
      ppConstTable);

    QueryPerformanceCounter(&compileEnd);
    double compileMs = (double)(compileEnd.QuadPart - compileStart.QuadPart) * 1000.0 / (double)compileFreq;

    DLOG_VERBOSE("DX12: D3DCompile: hr=0x%08X  %.1f ms  profile=%s  textLen=%d", (unsigned)hresult, compileMs, szProfile, len);
    if (compileMs > 500.0) {
      DLOG_VERBOSE("DX12: D3DCompile: SLOW shader compilation (%.1f ms)", compileMs);
    }

    if (D3D_OK != hresult) {
      failed = true;
    }
    // before we totally fail, let's try using ps_2_b instead of ps_2_a
    if (failed && !strcmp(szProfile, "ps_2_a")) {
      SafeRelease(m_pShaderCompileErrors);
      if (D3D_OK == D3DXCompileShader(szShaderText, len, NULL, NULL, szFn,
        "ps_2_b", m_dwShaderFlags, &pShaderByteCode, &m_pShaderCompileErrors, ppConstTable)) {
        failed = false;
      }
    }

    if (failed) {
      if (m_pShaderCompileErrors) {
        const char* errorMsg = (const char*)m_pShaderCompileErrors->GetBufferPointer();
        // Sized from the message, not a fixed 1024: MultiByteToWideChar writes
        // nothing and returns 0 when the buffer is too small, so a long error
        // list used to be reported as whatever was on the stack.
        std::wstring wide;
        int cch = MultiByteToWideChar(CP_ACP, 0, errorMsg, -1, NULL, 0);
        if (cch > 1) {
          wide.resize((size_t)cch - 1);
          MultiByteToWideChar(CP_ACP, 0, errorMsg, -1, &wide[0], cch);
        }
        wchar_t* wideErrorMsg = wide.empty() ? const_cast<wchar_t*>(L"") : &wide[0];
        dumpmsg(wideErrorMsg, LOG_ERROR);
        // Kept for the Preset Editor: it reads this to show the failure and,
        // via MapCompiledLineToUserLine(), to mark the offending source line.
        m_wLastShaderError = wide;

        // Write D3DCompile error to diagnostic file for Shader Import window
        if (shaderType == SHADER_COMP || shaderType == SHADER_WARP) {
          const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
          wchar_t diagName[64];
          FormatTo(diagName, L"diag_%hs_shader_error.txt", typeName);
          FILE* ef = DebugLogDiagOpen(diagName, L"w");
          if (ef) {
            // The preset being COMPILED, not the one on screen -- the same
            // distinction AutoFlagPresetError below already makes, and for the
            // same reason: compilation runs on the async load thread while the
            // previous preset is still live, so m_pState->m_szDesc names the
            // innocent preset that happened to precede the broken one.
            //
            // This header was the one place that still got it wrong, and it is
            // the header a human actually reads, because it sits on the FAILURE
            // dump. #141 records the cost: an attempt to migrate the FFT
            // helpers into preset text was diagnosed against a preset that was
            // not under test, because this line named it. The OK dump above has
            // preferred m_szLoadingPreset all along.
            const wchar_t* presetTag = L"(unknown)";
            if (m_szLoadingPreset[0])
              presetTag = m_szLoadingPreset;
            else if (m_pState && m_pState->m_szDesc[0])
              presetTag = m_pState->m_szDesc;
            fprintf(ef, "// DIAG: type=%s profile=%s shaderLen=%d preset=%ls\n",
                    typeName, szProfile, len, presetTag);
            fputs(errorMsg, ef);
            fclose(ef);
          }
        }

        SafeRelease(m_pShaderCompileErrors);
        AddNotification(wideErrorMsg);
        // The preset being COMPILED, not the one on screen. Shader compilation
        // happens on the async load thread while the previous preset is still
        // live, so m_szCurrentPresetFile named the innocent preset that
        // happened to precede the broken one -- and since the flag is written
        // straight to presets.json, a single bad preset left a permanent
        // "error" mark on whatever the user was watching before it. That is
        // how dozens of presets that compile perfectly came to be flagged.
        // #184: this site had the fix; its seven siblings did not, and each
        // still passed m_szCurrentPresetFile. Now one mechanism for all of
        // them -- and a thread_local one, so it stays correct when each render
        // context compiles its own preset on its own thread.
        AutoFlagPresetError(PresetFileForCompileError(), wide);
      }
      else {
        if (MessageBoxW(GetPluginWindow(), L"The shader could not be compiled.\n\nPlease install the Microsoft DirectX End-User Runtimes.\n\nOpen Download-Website now?", L"MDropDX12 Visualizer", MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
          // open website in browser
          ShellExecuteW(NULL, L"open", L"https://www.microsoft.com/en-us/download/details.aspx?id=35", NULL, NULL, SW_SHOWNORMAL);
        }
      }
      return false;
    }

    // This shader compiled, so the Preset Editor has nothing to complain about.
    m_wLastShaderError.clear();

    // Clear stale error file on successful compilation
    if (shaderType == SHADER_COMP || shaderType == SHADER_WARP) {
      const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
      wchar_t diagName[64];
      FormatTo(diagName, L"diag_%hs_shader_error.txt", typeName);
      // Prefer the preset being compiled (async load), not the live on-screen one
      const wchar_t* presetTag = L"(unknown)";
      if (m_pNewState && m_pNewState->m_szDesc[0] && m_nLoadingPreset > 0)
        presetTag = m_pNewState->m_szDesc;
      else if (m_szLoadingPreset[0])
        presetTag = m_szLoadingPreset;
      else if (m_pState && m_pState->m_szDesc[0])
        presetTag = m_pState->m_szDesc;
      char header[512];
      FormatToA(header, "// OK: type=%s len=%d preset=%ls loading=%d\n",
                typeName, len, presetTag, m_nLoadingPreset);
      DebugLogDiagWrite(diagName, header);
    }

    // Bytecode disassembly dump (verbose level only, for diagnosing SM5.0 codegen)
    if (g_debugLogLevel >= LOG_VERBOSE && pShaderByteCode &&
        (shaderType == SHADER_COMP || shaderType == SHADER_WARP)) {
      ID3DBlob* pDisasm = nullptr;
      if (SUCCEEDED(D3DDisassemble(pShaderByteCode->GetBufferPointer(),
                                    pShaderByteCode->GetBufferSize(), 0, nullptr, &pDisasm)) && pDisasm) {
        const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
        wchar_t diagName[64];
        FormatTo(diagName, L"diag_asm_%hs.txt", typeName);
        DebugLogDiagWrite(diagName, (const char*)pDisasm->GetBufferPointer());
        pDisasm->Release();
      }
    }

    if (m_ShaderCaching) {
      SaveShaderBytecodeToFile(pShaderByteCode, checksum, &szProfile[0]);
    }
  }

  // The DX9 CreateVertexShader/CreatePixelShader block that stood here is
  // gone (issue 17): never entered. *ppShader stays NULL as set above, and
  // no caller distinguishes -- the compile-validation gate is the branch
  // ABOVE this, which is live.

  // Store bytecode for DX12 PSO creation if requested
  if (ppBytecodeOut) {
    *ppBytecodeOut = pShaderByteCode; // transfer ownership
  } else {
    pShaderByteCode->Release();
  }
  pShaderByteCode = nullptr;

  return true;
}

void Engine::GenWarpPShaderText(char* szShaderText, bool bWrap) {
  // find the pixel shader body and replace it with custom code.
  // NOTE: decay is applied via vDiffuse.r (vertex color) which carries the
  // per-frame decay value. This allows per_frame code to override decay
  // dynamically (e.g. "decay = 0.92;"). DX9 used fixed-function vertex
  // color multiply for non-shader presets; this is the DX12 equivalent.

  // szShaderText is m_szWarpShadersText, char[MAX_BIGSTRING_LEN] (state.h).
  strncpy_s(szShaderText, MAX_BIGSTRING_LEN, m_szDefaultWarpPShaderText, _TRUNCATE);
  char LF = LINEFEED_CONTROL_CHAR;
  char* p = strrchr(szShaderText, '{');
  if (!p)
    return;
  p++;
  p += sprintf(p, "%c", 1);

  p += sprintf(p, "    // sample previous frame%c", LF);
  p += sprintf(p, "    ret = tex2D( sampler%ls_main, uv ).xyz;%c", bWrap ? L"" : L"_fc", LF);
  // Decay is applied in the output wrapper (ret *= _vDiffuse.rgb) for ALL warp shaders,
  // matching DX9's fixed-function texture stage modulate. Don't duplicate it here.
  p += sprintf(p, "}%c", LF);
}

void Engine::GenCompPShaderText(char* szShaderText, float hue_shader, bool bBrighten, bool bDarken, bool bSolarize, bool bInvert) {
  // find the pixel shader body and replace it with custom code.

  // szShaderText is m_szCompShadersText, char[MAX_SHADER_TEXT_LEN] (state.h).
  strncpy_s(szShaderText, MAX_SHADER_TEXT_LEN, m_szDefaultCompPShaderText, _TRUNCATE);
  char LF = LINEFEED_CONTROL_CHAR;
  char* p = strrchr(szShaderText, '{');
  if (!p)
    return;
  p++;
  p += sprintf(p, "%c", 1);

  // Video echo: read params from _c18 uniforms (updated every frame in ApplyShaderParams)
  p += sprintf(p, "    float ve_a = echo_alpha_param;%c", LF);
  p += sprintf(p, "    float ve_iz = echo_inv_zoom;%c", LF);
  p += sprintf(p, "    int ve_o = (int)echo_orient_param;%c", LF);
  p += sprintf(p, "    if (ve_a > 0.001) {%c", LF);
  p += sprintf(p, "        int ox = (ve_o %% 2) ? -1 : 1;%c", LF);
  p += sprintf(p, "        int oy = (ve_o >= 2) ? -1 : 1;%c", LF);
  p += sprintf(p, "        float2 uv_echo = (uv - 0.5) * ve_iz * float2(ox, oy) + 0.5;%c", LF);
  p += sprintf(p, "        ret = lerp(tex2D(sampler_main, uv).xyz,%c", LF);
  p += sprintf(p, "                   tex2D(sampler_main, uv_echo).xyz,%c", LF);
  p += sprintf(p, "                   ve_a); //video echo%c", LF);
  p += sprintf(p, "    } else {%c", LF);
  p += sprintf(p, "        ret = tex2D(sampler_main, uv).xyz;%c", LF);
  p += sprintf(p, "    }%c", LF);
  p += sprintf(p, "    ret *= gamma_adj; //gamma%c", LF);
  if (hue_shader >= 1.0f)
    p += sprintf(p, "    ret *= hue_shader; //old hue shader effect%c", LF);
  else if (hue_shader > 0.001f)
    p += sprintf(p, "    ret *= %.2f + %.2f*hue_shader; //old hue shader effect%c", 1 - hue_shader, hue_shader, LF);

  if (bBrighten)
    p += sprintf(p, "    ret = sqrt(ret); //brighten%c", LF);
  if (bDarken)
    p += sprintf(p, "    ret *= ret; //darken%c", LF);
  if (bSolarize)
    p += sprintf(p, "    ret = ret*(1-ret)*4; //solarize%c", LF);
  if (bInvert)
    p += sprintf(p, "    ret = 1 - ret; //invert%c", LF);
  //p += sprintf(p, "    ret.w = vDiffuse.w; // pass alpha along - req'd for preset blending%c", LF);
  p += sprintf(p, "}%c", LF);
}

// The shader cache directory, ABSOLUTE (#289).
//
// This was the bare relative string "cache", which Windows resolves against the
// process CURRENT DIRECTORY -- and a Win32 common file dialog changes that to
// whatever folder it was last browsing unless OFN_NOCHANGEDIR is set. Twenty-two
// of this app's dialogs did not set it. So opening Save.../Load... in the
// Displays window moved the CWD to resources\profiles\display\, and every
// shader compiled from then on was written to, and looked for in, a "cache"
// folder underneath it. Found on the live install: 467 .shader files, 4.2 MB,
// sitting in resources\profiles\display\cache\ -- every one of them a cache
// miss for the real directory.
//
// The dialogs get OFN_NOCHANGEDIR as well, but that is the belt and this is the
// braces: an absolute path cannot be moved by a CWD change that has not been
// thought of yet, and the precompile path in App.cpp already built it this way.
std::string Engine::ShaderCacheDir() const {
  std::wstring dir = m_szBaseDir;
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/')
    dir += L'\\';
  dir += L"cache";
  // Narrow, because the callers here use std::ofstream/ifstream on char paths.
  // The base directory is the install path; a non-ANSI one would have broken
  // the relative form too, and everything else in this file already assumes it.
  const int n = WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return std::string("cache");
  std::string out((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_ACP, 0, dir.c_str(), -1, &out[0], n, nullptr, nullptr);
  return out;
}

void Engine::SaveShaderBytecodeToFile(ID3DXBuffer* pShaderByteCode, uint32_t checksum, char* prefix) {
  if (!pShaderByteCode || !checksum) return;

  // Ensure the cache directory exists
  const std::string cacheDir = ShaderCacheDir();
  if (_mkdir(cacheDir.c_str()) != 0 && errno != EEXIST) {
    std::cerr << "Failed to create or access cache directory: " << cacheDir << std::endl;
    return;
  }
  std::ostringstream filePath;
  filePath << cacheDir << "\\" << prefix << "-" << std::hex << std::uppercase << checksum << ".shader";

  std::ofstream outFile(filePath.str(), std::ios::binary);
  if (outFile.is_open()) {
    outFile.write(
      static_cast<const char*>(pShaderByteCode->GetBufferPointer()),
      pShaderByteCode->GetBufferSize()
    );
    outFile.flush();
    outFile.close();
  }
}

ID3DXBuffer* Engine::LoadShaderBytecodeFromFile(uint32_t checksum, char* prefix) {
  ID3DXBuffer* pBuffer = nullptr;

  std::ostringstream filePath;
  filePath << ShaderCacheDir() << "\\" << prefix << "-" << std::hex
           << std::uppercase << checksum << ".shader";

  std::ifstream inFile(filePath.str(), std::ios::binary | std::ios::ate);
  if (!inFile.is_open()) return nullptr;

  std::streamsize size = inFile.tellg();
  inFile.seekg(0, std::ios::beg);

  if (SUCCEEDED(D3DXCreateBuffer((UINT)size, &pBuffer))) {
    char* dest = static_cast<char*>(pBuffer->GetBufferPointer());
    if (!inFile.read(dest, size)) {
      pBuffer->Release();
      return nullptr;
    }
  }

  return pBuffer;
}

uint32_t Engine::crc32(const char* data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint8_t>(data[i]);
    for (int j = 0; j < 8; ++j) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
  }
  return ~crc;
}

// Undo the line shift LoadShaderFromMemory introduced, so a D3DCompile error
// can be pointed at the line the preset author wrote.  Returns 0 when the
// compiled line lies in include.fx or the generated #defines -- there is no
// user line to blame in that case.
int Engine::MapCompiledLineToUserLine(int nCompiledLine) const {
  if (nCompiledLine <= m_nShaderPreludeLines)
    return 0;
  int nInjectedBefore = 0;
  for (int inj : m_shaderInjectedLines)
    if (inj <= nCompiledLine) nInjectedBefore++;
  const int userLine = nCompiledLine - m_nShaderPreludeLines - nInjectedBefore;
  return (userLine >= 1) ? userLine : 0;
}

} // namespace mdrop
