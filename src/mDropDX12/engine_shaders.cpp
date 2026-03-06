/*
  Plugin module: Shader Compilation & Caching
  Extracted from engine.cpp for maintainability.
  Contains: VShaderInfo, PShaderInfo, CShaderParams, RecompileVShader, RecompilePShader,
            LoadShaders, CreateDX12PresetPSOs, LoadShaderFromMemory, GenWarpPShaderText,
            GenCompPShaderText, SaveShaderBytecodeToFile, LoadShaderBytecodeFromFile, crc32
*/

#include "engine.h"
#include "engine_helpers.h"
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

// global_CShaderParams_master_list: a master list of all CShaderParams classes in existence.
//   ** when we evict a texture, we need to NULL out any texptrs these guys have! **
CShaderParamsList global_CShaderParams_master_list;
CShaderParams::CShaderParams() {
  if (global_CShaderParams_master_list.size() > 0)
    global_CShaderParams_master_list.push_back(this);
}

CShaderParams::~CShaderParams() {
  auto first = global_CShaderParams_master_list.begin();

  int N = (int)global_CShaderParams_master_list.size();
  for (int i = 0; i < N; i++)
    if (global_CShaderParams_master_list[i] == this)
      global_CShaderParams_master_list.erase(first + i);
  texsize_params.clear();
}

void CShaderParams::OnTextureEvict(LPDIRECT3DBASETEXTURE9 texptr) {
  for (int i = 0; i < sizeof(m_texture_bindings) / sizeof(m_texture_bindings[0]); i++)
    if (m_texture_bindings[i].texptr == texptr)
      m_texture_bindings[i].texptr = NULL;
}

void CShaderParams::Clear() {
  // float4 handles:
  rand_frame = NULL;
  rand_preset = NULL;

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

  {
    char dbg[256];
    sprintf(dbg, "DX12: CacheParams: %u constants", d.Constants);
    DebugLogA(dbg, LOG_VERBOSE);
  }

  // pass 1: find all the samplers (and texture bindings).
  for (UINT i = 0; i < d.Constants; i++) {
    D3DXHANDLE h = pCT->GetConstant(NULL, i);
    unsigned int count = 1;
    pCT->GetConstantDesc(h, &cd, &count);

    {
      char dbg[256];
      sprintf(dbg, "DX12: CacheParams pass1: [%u] Name=%s RegSet=%d RegIdx=%d", i, cd.Name ? cd.Name : "(null)", cd.RegisterSet, cd.RegisterIndex);
      DebugLogA(dbg, LOG_VERBOSE);
    }

    // cd.Name          = VS_Sampler
    // cd.RegisterSet   = D3DXRS_SAMPLER
    // cd.RegisterIndex = 3
    if (cd.RegisterSet == D3DXRS_SAMPLER && cd.RegisterIndex >= 0 && cd.RegisterIndex < sizeof(m_texture_bindings) / sizeof(m_texture_bindings[0])) {
      assert(m_texture_bindings[cd.RegisterIndex].texptr == NULL);

      // remove "sampler_" prefix to create root file name.  could still have "FW_" prefix or something like that.
      wchar_t szRootName[MAX_PATH];
      if (!strncmp(cd.Name, "sampler_", 8))
        lstrcpyW(szRootName, AutoWide(&cd.Name[8]));
      else
        lstrcpyW(szRootName, AutoWide(cd.Name));

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
        int i = 0;
        while (szRootName[i + 3]) {
          szRootName[i] = szRootName[i + 3];
          i++;
        }
        szRootName[i] = 0;
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
            lstrcpyW(prefix, &szRootName[7]);
          else
            prefix[0] = 0;
          szRootName[6] = 0;

          swscanf(&szRootName[4], L"%d", &rand_slot);
          if (rand_slot >= 0 && rand_slot <= 15)      // otherwise, not a special filename - ignore it
          {
            if (!PickRandomTexture(prefix, szRootName)) {
              if (prefix[0])
                swprintf(szRootName, L"[rand%02d] %s*", rand_slot, prefix);
              else
                swprintf(szRootName, L"[rand%02d] *", rand_slot);
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
          wcsncpy(x.texname, szRootName, 254);
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

          if (!g_engine.GetDevice()) {
            // DX12 path: load via WIC
            wchar_t szFilename[MAX_PATH];
            bool found = false;
            {
              char dbg[512];
              sprintf(dbg, "CacheParams: searching for texture '%ls'", szRootName);
              DebugLogA(dbg, LOG_VERBOSE);
            }
            for (int z = 0; z < texture_exts_count; z++) {
              swprintf(szFilename, L"%stextures\\%s.%s", g_engine.m_szMilkdrop2Path, szRootName, texture_exts[z].c_str());
              if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                swprintf(szFilename, L"%s%s.%s", g_engine.m_szPresetDir, szRootName, texture_exts[z].c_str());
                if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                  // Check for textures\ sibling of preset directory
                  bool siblingFound = false;
                  {
                    wchar_t siblingTexDir[MAX_PATH];
                    wcscpy_s(siblingTexDir, MAX_PATH, g_engine.m_szPresetDir);
                    int len = (int)wcslen(siblingTexDir);
                    if (len > 0 && siblingTexDir[len - 1] == L'\\') siblingTexDir[--len] = 0;
                    wchar_t* lastSlash = wcsrchr(siblingTexDir, L'\\');
                    if (lastSlash) {
                      wcscpy(lastSlash + 1, L"textures\\");
                      swprintf(szFilename, L"%s%s.%s", siblingTexDir, szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) siblingFound = true;
                    }
                  }
                  if (!siblingFound) {
                    // Search content base path first
                    bool fbFound = false;
                    if (g_engine.m_szContentBasePath[0]) {
                      swprintf(szFilename, L"%s%s.%s", g_engine.m_szContentBasePath, szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) fbFound = true;
                    }
                    // Then search fallback paths (paths already have trailing backslash)
                    if (!fbFound) {
                      for (auto& fbPath : g_engine.m_fallbackPaths) {
                        swprintf(szFilename, L"%s%s.%s", fbPath.c_str(), szRootName, texture_exts[z].c_str());
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
                x.nSizeInBytes = x.w * x.h * 4 + 16384;
                found = true;
                char dbg[512];
                sprintf(dbg, "CacheParams: loaded texture '%ls' from '%ls'", szRootName, szFilename);
                DebugLogA(dbg, LOG_VERBOSE);
                break;
              }
              // WIC couldn't decode this format (e.g. .dds) — try next extension
            }

            if (!found) {
              wchar_t buf[2048], title[64];
              swprintf(buf, wasabiApiLangString(IDS_COULD_NOT_LOAD_TEXTURE_X), szRootName, szExtsWithSlashes);
              g_engine.dumpmsg(buf, LOG_WARN);
              {
                char dbg[512];
                sprintf(dbg, "CacheParams: texture NOT found: '%ls' (base='%ls', preset='%ls', %d fallback paths)",
                        szRootName, g_engine.m_szMilkdrop2Path, g_engine.m_szPresetDir,
                        (int)g_engine.m_fallbackPaths.size());
                DebugLogA(dbg, LOG_VERBOSE);
              }
              if (bHardErrors)
                MessageBoxW(g_engine.GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
              else
                g_engine.AddError(buf, 6.0f, ERR_PRESET, true);
              continue;
            }

            g_engine.m_textures.push_back(x);
            m_texture_bindings[cd.RegisterIndex].dx12SrvIndex = x.dx12Tex.srvIndex;
          } else {
            // DX9 path: original D3DX texture loading

            // check if we need to evict anything from the cache,
            // due to our own cache constraints...
            while (1) {
              int nTexturesCached = 0;
              int nBytesCached = 0;
              int N = (int)g_engine.m_textures.size();
              for (int i = 0; i < N; i++)
                if (g_engine.m_textures[i].bEvictable && g_engine.m_textures[i].texptr) {
                  nBytesCached += g_engine.m_textures[i].nSizeInBytes;
                  nTexturesCached++;
                }
              if (nTexturesCached < g_engine.m_nMaxImages &&
                nBytesCached < g_engine.m_nMaxBytes)
                break;
              if (!g_engine.EvictSomeTexture())
                break;
            }

            //load the texture
            wchar_t szFilename[MAX_PATH];
            for (int z = 0; z < texture_exts_count; z++) {
              swprintf(szFilename, L"%stextures\\%s.%s", g_engine.m_szMilkdrop2Path, szRootName, texture_exts[z].c_str());
              if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                swprintf(szFilename, L"%s%s.%s", g_engine.m_szPresetDir, szRootName, texture_exts[z].c_str());
                if (GetFileAttributesW(szFilename) == 0xFFFFFFFF) {
                  // Search content base path first
                  bool fbFound = false;
                  if (g_engine.m_szContentBasePath[0]) {
                    swprintf(szFilename, L"%s%s.%s", g_engine.m_szContentBasePath, szRootName, texture_exts[z].c_str());
                    if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) fbFound = true;
                  }
                  // Then search fallback paths (paths already have trailing backslash)
                  if (!fbFound) {
                    for (auto& fbPath : g_engine.m_fallbackPaths) {
                      swprintf(szFilename, L"%s%s.%s", fbPath.c_str(), szRootName, texture_exts[z].c_str());
                      if (GetFileAttributesW(szFilename) != 0xFFFFFFFF) { fbFound = true; break; }
                    }
                  }
                  if (!fbFound) continue;
                }
              }
              D3DXIMAGE_INFO desc;

              while (1) {
                HRESULT hr = D3DXCreateTextureFromFileExW(g_engine.GetDevice(),
                  szFilename,
                  D3DX_DEFAULT_NONPOW2, D3DX_DEFAULT_NONPOW2,
                  D3DX_DEFAULT, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT,
                  D3DX_DEFAULT, D3DX_DEFAULT, 0, &desc, NULL,
                  (IDirect3DTexture9**)&x.texptr);
                if (hr == D3DERR_OUTOFVIDEOMEMORY || hr == E_OUTOFMEMORY) {
                  if (g_engine.EvictSomeTexture())
                    continue;
                }
                if (hr == D3D_OK) {
                  x.w = desc.Width;
                  x.h = desc.Height;
                  x.d = desc.Depth;
                  x.bEvictable = true;
                  x.nAge = g_engine.m_nPresetsLoadedTotal;
                  int nPixels = desc.Width * desc.Height * max(1, desc.Depth);
                  int BitsPerPixel = GetDX9TexFormatBitsPerPixel(desc.Format);
                  x.nSizeInBytes = nPixels * BitsPerPixel / 8 + 16384;
                }
                break;
              }
            }

            if (!x.texptr) {
              wchar_t buf[2048], title[64];
              swprintf(buf, wasabiApiLangString(IDS_COULD_NOT_LOAD_TEXTURE_X), szRootName, szExtsWithSlashes);
              g_engine.dumpmsg(buf, LOG_WARN);
              if (bHardErrors)
                MessageBoxW(g_engine.GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
              else
                g_engine.AddError(buf, 6.0f, ERR_PRESET, true);
              continue;
            }

            g_engine.m_textures.push_back(x);
            m_texture_bindings[cd.RegisterIndex].texptr = x.texptr;
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

    {
      char dbg[256];
      sprintf(dbg, "DX12: CacheParams pass2: [%u] Name=%s RegSet=%d Class=%d", i, cd.Name ? cd.Name : "(null)", cd.RegisterSet, cd.Class);
      DebugLogA(dbg, LOG_VERBOSE);
    }

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
        if (!strcmp(cd.Name, "rand_frame"))  rand_frame = h;
        else if (!strcmp(cd.Name, "rand_preset")) rand_preset = h;
        else if (!strncmp(cd.Name, "texsize_", 8)) {
          // remove "texsize_" prefix to find root file name.
          wchar_t szRootName[MAX_PATH];
          if (!strncmp(cd.Name, "texsize_", 8))
            lstrcpyW(szRootName, AutoWide(&cd.Name[8]));
          else
            lstrcpyW(szRootName, AutoWide(cd.Name));

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
                lstrcpyW(szRootName, RandTexName[rand_slot].c_str());
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

          if (!bTexFound && g_engine.GetDevice()) {
            // Only warn when DX9 device is available — in DX12 mode, noise textures
            // are not created so texsize_noise_* can't be resolved, which is expected.
            wchar_t buf[1024];
            swprintf(buf, wasabiApiLangString(IDS_UNABLE_TO_RESOLVE_TEXSIZE_FOR_A_TEXTURE_NOT_IN_USE), cd.Name);
            g_engine.AddError(buf, 6.0f, ERR_PRESET, true);
          }
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

  DebugLogA("DX12: CacheParams: pass 2 done, returning", LOG_VERBOSE);
}

//----------------------------------------------------------------------

bool Engine::RecompileVShader(const char* szShadersText, VShaderInfo* si, int shaderType, bool bHardErrors, bool bCompileOnly) {
  si->Clear();

  char ver[16];
  lstrcpy(ver, "vs_1_1");

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
  lstrcpy(ver, "ps_0_0");
  switch (PSVersion) {
  case MD2_PS_NONE:
    // Even though the PRESET doesn't use shaders, if MilkDrop is running where it CAN do shaders,
    //   we run all the old presets through (shader) emulation.
    // This way, during a MilkDrop session, we are always calling either WarpedBlit() or WarpedBlit_NoPixelShaders(),
    //   and blending always works.
    lstrcpy(ver, "ps_2_0");
    break;
  case MD2_PS_2_0: lstrcpy(ver, "ps_2_0"); break;
  case MD2_PS_2_X: lstrcpy(ver, "ps_2_a"); break; // we'll try ps_2_a first, LoadShaderFromMemory will try ps_2_b if compilation fails
  case MD2_PS_3_0: lstrcpy(ver, "ps_3_0"); break;
  case MD2_PS_4_0: lstrcpy(ver, "ps_4_0"); break;
  case MD2_PS_5_0: lstrcpy(ver, "ps_5_0"); break;
  default: assert(0); break;
  }

  if (!LoadShaderFromMemory(szShadersText, "PS", ver, &si->CT, (void**)&si->ptr, shaderType, bHardErrors, bCompileOnly, &si->bytecodeBlob, szDiagName)) {
    DebugLogA("DX12: RecompilePShader: LoadShaderFromMemory FAILED", LOG_ERROR);
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
  if (m_nMaxPSVersion <= 0) {
    DebugLogA("DX12: LoadShaders: m_nMaxPSVersion <= 0, skipping", LOG_VERBOSE);
    return true;
  }

  // load one of the pixel shaders
  {
    char dbg[256];
    sprintf(dbg, "DX12: LoadShaders: warp.ptr=%p warp.CT=%p nWarpPSVersion=%d nMaxPS=%d",
            (void*)sh->warp.ptr, (void*)sh->warp.CT, pState->m_nWarpPSVersion, m_nMaxPSVersion);
    DebugLogA(dbg, LOG_VERBOSE);
  }
  if (!sh->warp.ptr && !sh->warp.CT && pState->m_nWarpPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szWarpShadersText, &sh->warp, SHADER_WARP, false, pState->m_nWarpPSVersion, bCompileOnly);
    {
      char dbg[256];
      sprintf(dbg, "DX12: LoadShaders warp: bOK=%d bytecodeBlob=%p CT=%p ptr=%p",
              bOK, (void*)sh->warp.bytecodeBlob, (void*)sh->warp.CT, (void*)sh->warp.ptr);
      DebugLogA(dbg, LOG_VERBOSE);
    }
    if (!bOK) {
      // switch to fallback shader
      if (m_fallbackShaders_ps.warp.ptr) m_fallbackShaders_ps.warp.ptr->AddRef();
      if (m_fallbackShaders_ps.warp.CT) m_fallbackShaders_ps.warp.CT->AddRef();
      if (m_fallbackShaders_ps.warp.bytecodeBlob) m_fallbackShaders_ps.warp.bytecodeBlob->AddRef();
      memcpy(&sh->warp, &m_fallbackShaders_ps.warp, sizeof(PShaderInfo));
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

  // Comp (Image) shader — compiled after bufferA/bufferB so diag_comp_shader.txt reflects comp
  if (!sh->comp.ptr && !sh->comp.CT && pState->m_nCompPSVersion > 0) {
    bool bOK = RecompilePShader(pState->m_szCompShadersText, &sh->comp, SHADER_COMP, false, pState->m_nCompPSVersion, bCompileOnly);
    {
      char dbg[256];
      sprintf(dbg, "DX12: LoadShaders comp: bOK=%d bytecodeBlob=%p CT=%p ptr=%p",
              bOK, (void*)sh->comp.bytecodeBlob, (void*)sh->comp.CT, (void*)sh->comp.ptr);
      DebugLogA(dbg, LOG_VERBOSE);
    }
    if (!bOK) {
      // switch to fallback shader
      if (m_fallbackShaders_ps.comp.ptr) m_fallbackShaders_ps.comp.ptr->AddRef();
      if (m_fallbackShaders_ps.comp.CT) m_fallbackShaders_ps.comp.CT->AddRef();
      if (m_fallbackShaders_ps.comp.bytecodeBlob) m_fallbackShaders_ps.comp.bytecodeBlob->AddRef();
      memcpy(&sh->comp, &m_fallbackShaders_ps.comp, sizeof(PShaderInfo));
    }
  }

  return true;
}

void Engine::CreateDX12PresetPSOs() {
  if (!m_lpDX || !m_lpDX->m_device.Get() || !m_lpDX->m_rootSignature.Get())
    return;

  // Wait for GPU to finish all in-flight command lists before releasing old PSOs.
  // Without this, the GPU may still be executing a previous frame's command list
  // that references the old PSOs — releasing them causes use-after-free / TDR.
  m_lpDX->WaitForGpu();

  ID3D12Device* device = m_lpDX->m_device.Get();
  ID3D12RootSignature* rootSig = m_lpDX->m_rootSignature.Get();
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  // Create warp PSO from current shader bytecode
  m_dx12WarpPSO.Reset();
  m_warpMainTexSlot = 0;
  if (m_shaders.warp.bytecodeBlob && g_pWarpVSBlob) {
    m_dx12WarpPSO = DX12CreatePresetPSO(
      device, rootSig, rtvFormat,
      g_pWarpVSBlob,
      m_shaders.warp.bytecodeBlob->GetBufferPointer(),
      (UINT)m_shaders.warp.bytecodeBlob->GetBufferSize(),
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
  m_dx12CompPSO.Reset();
  m_compMainTexSlot = 0;
  if (m_shaders.comp.bytecodeBlob && g_pCompVSBlob) {
    m_dx12CompPSO = DX12CreatePresetPSO(
      device, rootSig, compRtvFormat,
      g_pCompVSBlob,
      m_shaders.comp.bytecodeBlob->GetBufferPointer(),
      (UINT)m_shaders.comp.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &m_compMainTexSlot);
    if (m_compMainTexSlot == UINT_MAX) m_compMainTexSlot = 0;
  }

  // Create Buffer A PSO — always renders to FLOAT32 feedback buffer (Shadertoy uses float32)
  DXGI_FORMAT feedbackRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
  m_dx12BufferAPSO.Reset();
  if (m_shaders.bufferA.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    m_dx12BufferAPSO = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      m_shaders.bufferA.bytecodeBlob->GetBufferPointer(),
      (UINT)m_shaders.bufferA.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  // Create Buffer B PSO — same FLOAT32 feedback format as Buffer A
  m_dx12BufferBPSO.Reset();
  if (m_shaders.bufferB.bytecodeBlob && g_pCompVSBlob) {
    UINT dummy = 0;
    m_dx12BufferBPSO = DX12CreatePresetPSO(
      device, rootSig, feedbackRtvFormat,
      g_pCompVSBlob,
      m_shaders.bufferB.bytecodeBlob->GetBufferPointer(),
      (UINT)m_shaders.bufferB.bytecodeBlob->GetBufferSize(),
      g_MyVertexLayout, _countof(g_MyVertexLayout),
      false, &dummy);
  }

  {
    char dbg[256];
    sprintf(dbg, "DX12: Preset warp PSO: %s (mainTexSlot=%u)", m_dx12WarpPSO ? "OK" : "FALLBACK", m_warpMainTexSlot);
    DebugLogA(dbg, LOG_VERBOSE);
    sprintf(dbg, "DX12: Preset comp PSO: %s (mainTexSlot=%u)", m_dx12CompPSO ? "OK" : "FALLBACK", m_compMainTexSlot);
    DebugLogA(dbg, LOG_VERBOSE);
    if (m_dx12BufferAPSO)
      DebugLogA("DX12: Preset bufferA PSO: OK");
    if (m_dx12BufferBPSO)
      DebugLogA("DX12: Preset bufferB PSO: OK");
  }
}

// Preprocessor: fix matrix * vector multiplication.
// HLSL requires mul() for matrix-vector multiply; the * operator causes X3020 type mismatch.
// Finds variables declared as float2x2/float3x3/float4x4 and wraps their * operations.
static void FixMatrixVarMultiply(char* szShaderText) {
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

  for (auto& mt : matTypes) {
    int mtLen = (int)strlen(mt);
    const char* s = szShaderText;
    while ((s = strstr(s, mt)) != NULL) {
      if (s > szShaderText && isIdent(s[-1])) { s += mtLen; continue; }
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

  if (nMatVars == 0) return;

  // Phase 2: replace matVar*ident → mul(matVar, ident) and ident*matVar → mul(ident, matVar)
  int srcLen = (int)strlen(szShaderText);
  char* tmp = (char*)malloc(srcLen + 32768);
  if (!tmp) return;

  for (int mi = 0; mi < nMatVars; mi++) {
    const char* mv = matVars[mi];
    int mvLen = matVarLens[mi];
    int wi = 0;

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
            // If the identifier is followed by '(' it's a function call — include the args
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
            memcpy(&tmp[wi], "mul(", 4); wi += 4;
            memcpy(&tmp[wi], mv, mvLen); wi += mvLen;
            memcpy(&tmp[wi], ", ", 2); wi += 2;
            memcpy(&tmp[wi], &szShaderText[opStart], afterStar - opStart);
            wi += afterStar - opStart;
            tmp[wi++] = ')';
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
            // Include preceding ident.swizzle pattern (e.g., n.yzw → capture full "n.yzw")
            if (opStart > 1 && szShaderText[opStart - 1] == '.') {
              int dotPos = opStart - 1;
              int identStart = dotPos;
              while (identStart > 0 && isIdent(szShaderText[identStart - 1])) identStart--;
              if (identStart < dotPos)
                opStart = identStart;  // include "n." prefix
            }
            if (opEnd > opStart) {
              // Rewind output to before operand * matVar
              wi -= (i - opStart);  // remove already-written "operand * " from tmp
              memcpy(&tmp[wi], "mul(", 4); wi += 4;
              memcpy(&tmp[wi], &szShaderText[opStart], opEnd - opStart);
              wi += opEnd - opStart;
              memcpy(&tmp[wi], ", ", 2); wi += 2;
              memcpy(&tmp[wi], mv, mvLen); wi += mvLen;
              tmp[wi++] = ')';
              i += mvLen;
              continue;
            }
          }
        }
        // No match — copy as-is
        memcpy(&tmp[wi], mv, mvLen);
        wi += mvLen;
        i += mvLen;
      } else {
        tmp[wi++] = szShaderText[i++];
      }
    }
    tmp[wi] = 0;
    memcpy(szShaderText, tmp, wi + 1);
    srcLen = wi;
  }

  free(tmp);
}

// Preprocessor: rename local variables that shadow HLSL built-in functions.
// Many MilkDrop presets use patterns like `float2 pow = float2(pow(x,y)...)` which
// fails in SM3.0+ because the local variable shadows the intrinsic. We rename the
// variable (non-function-call occurrences) to `_mw_<name>` while leaving `<name>(` calls intact.
static void FixShadowedBuiltins(char* szShaderText) {
  static const char* builtins[] = {
    "pow", "mul", "sin", "cos", "tan", "exp", "log", "dot",
    "abs", "min", "max", "step", "lerp", "frac", "sqrt",
    "floor", "ceil", "round", "sign", "clamp", "saturate",
    "normalize", "length", "distance", "cross", "clip",
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

  int srcLen = (int)strlen(szShaderText);
  // Temp buffer: each rename adds 4 chars ("_mw_" prefix). 128KB source buffer has plenty of room.
  char* tmp = (char*)malloc(srcLen + 32768);
  if (!tmp) return;

  for (int bi = 0; bi < nBuiltins; bi++) {
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
    int wi = 0;
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
          memcpy(&tmp[wi], name, nameLen);
          wi += nameLen;
          i += nameLen;
        } else {
          // Variable reference — rename to _mw_<name>
          memcpy(&tmp[wi], "_mw_", 4);
          wi += 4;
          memcpy(&tmp[wi], name, nameLen);
          wi += nameLen;
          i += nameLen;
        }
      } else {
        tmp[wi++] = szShaderText[i++];
      }
    }
    tmp[wi] = 0;

    // Copy back
    memcpy(szShaderText, tmp, wi + 1);
    srcLen = wi;
  }

  free(tmp);
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
    "#define uv_orig _uv.xy\n" //[sic]
    "#define hue_shader _vDiffuse.xyz\n";
  const char szWarpParams[] = "float4 _vDiffuse : COLOR, float4 _uv : TEXCOORD0, float2 _rad_ang : TEXCOORD1, out float4 _return_value : COLOR0";
  const char szCompParams[] = "float4 _vDiffuse : COLOR, float2 _uv : TEXCOORD0, float2 _rad_ang : TEXCOORD1, out float4 _return_value : COLOR0";
  const char szFirstLine[] = "    float3 ret = 0;";

  char szWhichShader[64];
  switch (shaderType) {
  case SHADER_WARP:  lstrcpy(szWhichShader, "warp"); break;
  case SHADER_COMP:  lstrcpy(szWhichShader, "composite"); break;
  case SHADER_BLUR:  lstrcpy(szWhichShader, "blur"); break;
  case SHADER_OTHER: lstrcpy(szWhichShader, "(other)"); break;
  default:           lstrcpy(szWhichShader, "(unknown)"); break;
  }

  LPD3DXBUFFER pShaderByteCode = NULL;
  wchar_t title[64];

  *ppShader = NULL;
  *ppConstTable = NULL;

  // DIAG: log original shader text (before include.fx prepend)
  {
    char dbg[512];
    int origLen = szOrigShaderText ? (int)strlen(szOrigShaderText) : 0;
    char preview[301] = {0};
    if (origLen > 0) {
      strncpy(preview, szOrigShaderText, 300);
      for (int i = 0; i < 300 && preview[i]; i++)
        if (preview[i] < 32 && preview[i] != 0) preview[i] = '|';
    }
    sprintf(dbg, "DIAG LoadShader: type=%d(%s) origLen=%d text='%.300s'",
            shaderType, szWhichShader, origLen, preview);
    DebugLogA(dbg, LOG_VERBOSE);
  }

  char szShaderText[128000];
  char temp[128000];
  int writePos = 0;

  // paste the universal #include
  lstrcpy(&szShaderText[writePos], m_szShaderIncludeText);  // first, paste in the contents of 'inputs.fx' before the actual shader text.  Has 13's and 10's.
  writePos += m_nShaderIncludeTextLen;

  // paste in any custom #defines for this shader type
  if (shaderType == SHADER_WARP && szProfile[0] == 'p') {
    lstrcpy(&szShaderText[writePos], szWarpDefines);
    writePos += lstrlen(szWarpDefines);
  }
  else if (shaderType == SHADER_COMP && szProfile[0] == 'p') {
    lstrcpy(&szShaderText[writePos], szCompDefines);
    writePos += lstrlen(szCompDefines);
  }
  // paste in the shader itself - converting LCC's to 13+10's.
  // avoid lstrcpy b/c it might not handle the linefeed stuff...?
  int shaderStartPos = writePos;
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
    {
      char dbg[256];
      sprintf(dbg, "DIAG shader_body search: type=%d found=%d offsetFromStart=%d",
              shaderType, (*p != 0) ? 1 : 0, (int)(p - &szShaderText[shaderStartPos]));
      DebugLogA(dbg, LOG_VERBOSE);
    }

    if (*p) {
      for (int i = 0; i < 11; i++)
        *p++ = ' ';
    }
    else {
      p = NULL; // shader_body not found — signal error
    }

    if (p) {
      // insert "void PS(...params...)\n"
      lstrcpy(temp, p);
      const char* params = (shaderType == SHADER_WARP) ? szWarpParams : szCompParams;
      sprintf(p, "void %s( %s )\n", szFn, params);
      p += lstrlen(p);
      lstrcpy(p, temp);

      // find the starting curly brace
      p = strchr(p, '{');
      if (p) {
        // skip over it
        p++;
        // then insert first line(s)
        lstrcpy(temp, p);
        if (m_bLoadingShadertoyMode && !bHardErrors) {
          // Shadertoy: float4 ret to preserve alpha channel (temporal accumulation data)
          // Use m_bLoadingShadertoyMode (set before async thread) not m_bShadertoyMode
          // (set after compilation in LoadPresetTick — too late for shader text generation)
          sprintf(p, "    float4 ret = 0;\n");
        } else {
          sprintf(p, "%s\n", szFirstLine);
        }
        p += lstrlen(p);
        lstrcpy(p, temp);

        // find the ending curly brace
        p = strrchr(p, '}');
        if (p) {
          if (m_bLoadingShadertoyMode && !bHardErrors) {
            // Shadertoy output: Buffer A/B preserve full float4 (alpha stores data);
            // Image/comp forces alpha=1 (shaders that write .rgb leave alpha=0 which
            // would be transparent — the old non-Shadertoy wrapper used _vDiffuse.w=1).
            bool bIsBuffer = szDiagName && (strcmp(szDiagName, "bufferA") == 0 || strcmp(szDiagName, "bufferB") == 0);
            const char* szLastLine = bIsBuffer
              ? "    _return_value = ret;"
              : "    _return_value = float4(ret.xyz, 1.0);";
            sprintf(p, " %s\n}\n", szLastLine);
          } else {
            // MilkDrop3 does NOT apply gamma_adj or B/D/S/I for custom comp shader presets.
            // gamma_adj is only used in ShowToUser_NoShaders path (no custom comp shader).
            // shiftHSV is an MDropDX12 addition for colshift; early-exits when colshift values are 0.
            char szLastLine[] = "    _return_value = float4(shiftHSV(ret.xyz), _vDiffuse.w);";
            sprintf(p, " %s\n}\n", szLastLine);
          }
        }
      }
    }

    if (!p) {
      wchar_t temp[512];
      swprintf(temp, wasabiApiLangString(IDS_ERROR_PARSING_X_X_SHADER), szProfile, szWhichShader);
      dumpmsg(temp, LOG_WARN);
      AddError(temp, 8.0f, ERR_PRESET, true);
      return false;
    }
  }

  // Fix variables that shadow HLSL built-in functions (e.g. float2 pow = ...)
  FixShadowedBuiltins(szShaderText);

  // Fix matrix * vector multiplication (HLSL requires mul())
  FixMatrixVarMultiply(szShaderText);

  // Dump assembled shader text to file for diagnostics (written to m_szBaseDir)
  if (shaderType == SHADER_COMP || shaderType == SHADER_WARP) {
    const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
    char dumpPath[MAX_PATH];
    sprintf(dumpPath, "%lsdiag_%s_shader.txt", m_szBaseDir, typeName);
    FILE* f = fopen(dumpPath, "w");
    if (f) {
      fprintf(f, "// DIAG: type=%s profile=%s len=%d preset=%ls\n",
              typeName, szProfile, lstrlen(szShaderText),
              m_pState ? m_pState->m_szDesc : L"(unknown)");
      fputs(szShaderText, f);
      fclose(f);
    }
  }

  // now really try to compile it.

  bool failed = false;
  int len = lstrlen(szShaderText);

  uint32_t checksum = crc32(szShaderText, len);

  {
    char dbg[256];
    sprintf(dbg, "DX12: LoadShaderFromMemory: checksum=0x%08X caching=%d", checksum, m_ShaderCaching);
    DebugLogA(dbg, LOG_VERBOSE);
  }

  if (m_ShaderCaching) {
    pShaderByteCode = LoadShaderBytecodeFromFile(checksum, &szProfile[0]);
    {
      char dbg[256];
      sprintf(dbg, "DX12: LoadShaderFromMemory: cache %s (bytecode=%p)", pShaderByteCode ? "HIT" : "MISS", (void*)pShaderByteCode);
      DebugLogA(dbg, LOG_VERBOSE);
    }
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

    {
      char dbg[256];
      sprintf(dbg, "DX12: D3DCompile: hr=0x%08X  %.1f ms  profile=%s  textLen=%d", (unsigned)hresult, compileMs, szProfile, len);
      DebugLogA(dbg, LOG_VERBOSE);
      if (compileMs > 500.0) {
        sprintf(dbg, "DX12: D3DCompile: SLOW shader compilation (%.1f ms)", compileMs);
        DebugLogA(dbg, LOG_VERBOSE);
      }
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
      wchar_t wideErrorMsg[1024];

      if (m_pShaderCompileErrors) {
        const char* errorMsg = (const char*)m_pShaderCompileErrors->GetBufferPointer();
        // Convert to wide string
        MultiByteToWideChar(CP_ACP, 0, errorMsg, -1, wideErrorMsg, _countof(wideErrorMsg));
        dumpmsg(wideErrorMsg, LOG_WARN);

        // Write D3DCompile error to diagnostic file for Shader Import window
        if (shaderType == SHADER_COMP || shaderType == SHADER_WARP) {
          const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
          char errPath[MAX_PATH];
          sprintf(errPath, "%lsdiag_%s_shader_error.txt", m_szBaseDir, typeName);
          FILE* ef = fopen(errPath, "w");
          if (ef) {
            fprintf(ef, "// DIAG: type=%s profile=%s shaderLen=%d preset=%ls\n",
                    typeName, szProfile, len,
                    m_pState ? m_pState->m_szDesc : L"(unknown)");
            fputs(errorMsg, ef);
            fclose(ef);
          }
        }

        SafeRelease(m_pShaderCompileErrors);
        AddNotification(wideErrorMsg);
      }
      else {
        if (MessageBoxA(GetPluginWindow(), "The shader could not be compiled.\n\nPlease install the Microsoft DirectX End-User Runtimes.\n\nOpen Download-Website now?", "MDropDX12 Visualizer", MB_YESNO | MB_SETFOREGROUND | MB_TOPMOST) == IDYES) {
          // open website in browser
          ShellExecuteA(NULL, "open", "https://www.microsoft.com/en-us/download/details.aspx?id=35", NULL, NULL, SW_SHOWNORMAL);
        }
      }
      return false;
    }

    // Clear stale error file on successful compilation
    if (shaderType == SHADER_COMP || shaderType == SHADER_WARP) {
      const char* typeName = szDiagName ? szDiagName : (shaderType == SHADER_COMP ? "comp" : "warp");
      char errPath[MAX_PATH];
      sprintf(errPath, "%lsdiag_%s_shader_error.txt", m_szBaseDir, typeName);
      FILE* ef = fopen(errPath, "w");
      if (ef) {
        fprintf(ef, "// OK: type=%s len=%d preset=%ls\n",
                typeName, len, m_pState ? m_pState->m_szDesc : L"(unknown)");
        fclose(ef);
      }
    }

    if (m_ShaderCaching) {
      SaveShaderBytecodeToFile(pShaderByteCode, checksum, &szProfile[0]);
    }
  }

  // load ok, create the shader
  if (!compileOnly && GetDevice()) {
    HRESULT hr = 1;
    if (szProfile[0] == 'v') {
      hr = GetDevice()->CreateVertexShader((const unsigned long*)(pShaderByteCode->GetBufferPointer()), (IDirect3DVertexShader9**)ppShader);
    }
    else if (szProfile[0] == 'p') {
      hr = GetDevice()->CreatePixelShader((const unsigned long*)(pShaderByteCode->GetBufferPointer()), (IDirect3DPixelShader9**)ppShader);
    }

    if (hr != D3D_OK) {
      wchar_t temp[512];
      wasabiApiLangString(IDS_ERROR_CREATING_SHADER, temp, sizeof(temp));
      // dumpmsg(temp);
      if (bHardErrors)
        MessageBoxW(GetPluginWindow(), temp, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
      else {
        AddError(temp, 6.0f, ERR_PRESET, true);
      }
      return false;
    }
  }

  // Store bytecode for DX12 PSO creation if requested
  if (ppBytecodeOut) {
    *ppBytecodeOut = pShaderByteCode; // transfer ownership
  } else {
    pShaderByteCode->Release();
  }
  pShaderByteCode = nullptr;

  return true;
}

void Engine::GenWarpPShaderText(char* szShaderText, float decay, bool bWrap) {
  // find the pixel shader body and replace it with custom code.

  lstrcpy(szShaderText, m_szDefaultWarpPShaderText);
  char LF = LINEFEED_CONTROL_CHAR;
  char* p = strrchr(szShaderText, '{');
  if (!p)
    return;
  p++;
  p += sprintf(p, "%c", 1);

  p += sprintf(p, "    // sample previous frame%c", LF);
  // SPOUT
  // Avoid freeze
  p += sprintf(p, "    ret = tex2D( sampler%ls_main, uv ).xyz;%c", bWrap ? L"" : L"_fc", LF);
  // p += sprintf(p, "    ret = tex2D( sampler%s_main, uv ).xyz;%c", bWrap ? L"" : L"_fc", LF);
  p += sprintf(p, "    %c", LF);
  p += sprintf(p, "    // darken (decay) over time%c", LF);
  p += sprintf(p, "    ret *= %.2f; //or try: ret -= 0.004;%c", decay, LF);
  //p += sprintf(p, "    %c", LF);
  //p += sprintf(p, "    ret.w = vDiffuse.w; // pass alpha along - req'd for preset blending%c", LF);
  p += sprintf(p, "}%c", LF);
}

void Engine::GenCompPShaderText(char* szShaderText, float brightness, float ve_alpha, float ve_zoom, int ve_orient, float hue_shader, bool bBrighten, bool bDarken, bool bSolarize, bool bInvert) {
  // find the pixel shader body and replace it with custom code.

  lstrcpy(szShaderText, m_szDefaultCompPShaderText);
  char LF = LINEFEED_CONTROL_CHAR;
  char* p = strrchr(szShaderText, '{');
  if (!p)
    return;
  p++;
  p += sprintf(p, "%c", 1);

  if (ve_alpha > 0.001f) {
    int orient_x = (ve_orient % 2) ? -1 : 1;
    int orient_y = (ve_orient >= 2) ? -1 : 1;
    p += sprintf(p, "    float2 uv_echo = (uv - 0.5)*%.3f*float2(%d,%d) + 0.5;%c", 1.0f / ve_zoom, orient_x, orient_y, LF);
    p += sprintf(p, "    ret = lerp( tex2D(sampler_main, uv).xyz, %c", LF);
    p += sprintf(p, "                tex2D(sampler_main, uv_echo).xyz, %c", LF);
    p += sprintf(p, "                %.2f %c", ve_alpha, LF);
    p += sprintf(p, "              ); //video echo%c", LF);
    p += sprintf(p, "    ret *= %.2f; //gamma%c", brightness, LF);
  }
  else {
    p += sprintf(p, "    ret = tex2D(sampler_main, uv).xyz;%c", LF);
    p += sprintf(p, "    ret *= %.2f; //gamma%c", brightness, LF);
  }
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

void Engine::SaveShaderBytecodeToFile(ID3DXBuffer* pShaderByteCode, uint32_t checksum, char* prefix) {
  if (!pShaderByteCode || !checksum) return;

  // Ensure the "cache" directory exists
  const char* cacheDir = "cache";
  if (_mkdir(cacheDir) != 0 && errno != EEXIST) {
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
  filePath << "cache\\" << prefix << "-" << std::hex << std::uppercase << checksum << ".shader";

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


} // namespace mdrop
