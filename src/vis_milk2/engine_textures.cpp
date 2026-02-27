/*
  Plugin module: Texture Management
  Extracted from engine.cpp for maintainability.
  Contains: AddNoiseTex, AddNoiseVol, EvictSomeTexture, PickRandomTexture
*/

#include "engine.h"
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
#include <vector>
#include <string>

namespace mdrop {

extern Engine g_engine;
extern CShaderParamsList global_CShaderParams_master_list;

// Forward declarations for helpers defined in engine.cpp
DWORD dwCubicInterpolate(DWORD y0, DWORD y1, DWORD y2, DWORD y3, float t);
float fCubicInterpolate(float y0, float y1, float y2, float y3, float t);

bool Engine::AddNoiseTex(const wchar_t* szTexName, int size, int zoom_factor) {
  if (!GetDevice()) {
    // DX12 path: generate noise into CPU buffer, then upload via DX12
    int RANGE = (zoom_factor > 1) ? 216 : 256;
    std::vector<DWORD> pixels(size * size);
    DWORD* dst = pixels.data();

    for (int y = 0; y < size; y++) {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      srand(q.LowPart ^ q.HighPart ^ rand());
      for (int x = 0; x < size; x++) {
        dst[y * size + x] = (((DWORD)(rand() % RANGE) + RANGE / 2) << 24) |
          (((DWORD)(rand() % RANGE) + RANGE / 2) << 16) |
          (((DWORD)(rand() % RANGE) + RANGE / 2) << 8) |
          (((DWORD)(rand() % RANGE) + RANGE / 2));
      }
      for (int x = 0; x < size; x++) {
        int x1 = (rand() ^ q.LowPart) % size;
        int x2 = (rand() ^ q.HighPart) % size;
        DWORD temp = dst[y * size + x2];
        dst[y * size + x2] = dst[y * size + x1];
        dst[y * size + x1] = temp;
      }
    }

    // cubic interpolation smoothing
    if (zoom_factor > 1) {
      for (int y = 0; y < size; y += zoom_factor)
        for (int x = 0; x < size; x++)
          if (x % zoom_factor) {
            int base_x = (x / zoom_factor) * zoom_factor + size;
            DWORD y0 = dst[y * size + ((base_x - zoom_factor) % size)];
            DWORD y1 = dst[y * size + ((base_x) % size)];
            DWORD y2 = dst[y * size + ((base_x + zoom_factor) % size)];
            DWORD y3 = dst[y * size + ((base_x + zoom_factor * 2) % size)];
            float t = (x % zoom_factor) / (float)zoom_factor;
            dst[y * size + x] = dwCubicInterpolate(y0, y1, y2, y3, t);
          }

      for (int x = 0; x < size; x++)
        for (int y = 0; y < size; y++)
          if (y % zoom_factor) {
            int base_y = (y / zoom_factor) * zoom_factor + size;
            DWORD y0 = dst[((base_y - zoom_factor) % size) * size + x];
            DWORD y1 = dst[((base_y) % size) * size + x];
            DWORD y2 = dst[((base_y + zoom_factor) % size) * size + x];
            DWORD y3 = dst[((base_y + zoom_factor * 2) % size) * size + x];
            float t = (y % zoom_factor) / (float)zoom_factor;
            dst[y * size + x] = dwCubicInterpolate(y0, y1, y2, y3, t);
          }
    }

    // Upload to GPU — D3DFMT_A8R8G8B8 byte layout matches DXGI_FORMAT_B8G8R8A8_UNORM
    TexInfo x;
    lstrcpyW(x.texname, szTexName);
    x.texptr = NULL;
    x.w = size;
    x.h = size;
    x.d = 1;
    x.bEvictable = false;
    x.nAge = m_nPresetsLoadedTotal;
    x.nSizeInBytes = size * size * 4;
    x.dx12Tex = m_lpDX->CreateTextureFromPixels(pixels.data(), size, size,
                                                 size * sizeof(DWORD),
                                                 DXGI_FORMAT_B8G8R8A8_UNORM);
    m_textures.push_back(x);
    return true;
  }
  // size = width & height of the texture;
  // zoom_factor = how zoomed-in the texture features should be.
  //           1 = random noise
  //           2 = smoothed (interp)
  //           4/8/16... = cubic interp.

  wchar_t buf[2048], title[64];

  // Synthesize noise texture(s)
  LPDIRECT3DTEXTURE9 pNoiseTex = NULL;
  // try twice - once with mips, once without.
  for (int i = 0; i < 2; i++) {
    if (D3D_OK != GetDevice()->CreateTexture(size, size, i, D3DUSAGE_DYNAMIC | (i ? 0 : D3DUSAGE_AUTOGENMIPMAP), D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pNoiseTex, NULL)) {
      if (i == 1) {
        wasabiApiLangString(IDS_COULD_NOT_CREATE_NOISE_TEXTURE, buf, sizeof(buf));
        dumpmsg(buf);
        MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
      }
    }
    else
      break;
  }

  D3DLOCKED_RECT r;
  if (D3D_OK != pNoiseTex->LockRect(0, &r, NULL, D3DLOCK_DISCARD)) {
    wasabiApiLangString(IDS_COULD_NOT_LOCK_NOISE_TEXTURE, buf, sizeof(buf));
    dumpmsg(buf);
    MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }

  if (r.Pitch < size * 4) {
    wasabiApiLangString(IDS_NOISE_TEXTURE_BYTE_LAYOUT_NOT_RECOGNISED, buf, sizeof(buf));
    dumpmsg(buf);
    MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }

  // write to the bits...
  DWORD* dst = (DWORD*)r.pBits;
  int dwords_per_line = r.Pitch / sizeof(DWORD);
  int RANGE = (zoom_factor > 1) ? 216 : 256;
  for (int y = 0; y < size; y++) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    srand(q.LowPart ^ q.HighPart ^ rand());
    for (int x = 0; x < size; x++) {
      dst[x] = (((DWORD)(rand() % RANGE) + RANGE / 2) << 24) |
        (((DWORD)(rand() % RANGE) + RANGE / 2) << 16) |
        (((DWORD)(rand() % RANGE) + RANGE / 2) << 8) |
        (((DWORD)(rand() % RANGE) + RANGE / 2));
    }
    // swap some pixels randomly, to improve 'randomness'
    for (int x = 0; x < size; x++) {
      int x1 = (rand() ^ q.LowPart) % size;
      int x2 = (rand() ^ q.HighPart) % size;
      DWORD temp = dst[x2];
      dst[x2] = dst[x1];
      dst[x1] = temp;
    }
    dst += dwords_per_line;
  }

  // smoothing
  if (zoom_factor > 1) {
    // first go ACROSS, blending cubically on X, but only on the main lines.
    DWORD* dst = (DWORD*)r.pBits;
    for (int y = 0; y < size; y += zoom_factor)
      for (int x = 0; x < size; x++)
        if (x % zoom_factor) {
          int base_x = (x / zoom_factor) * zoom_factor + size;
          int base_y = y * dwords_per_line;
          DWORD y0 = dst[base_y + ((base_x - zoom_factor) % size)];
          DWORD y1 = dst[base_y + ((base_x) % size)];
          DWORD y2 = dst[base_y + ((base_x + zoom_factor) % size)];
          DWORD y3 = dst[base_y + ((base_x + zoom_factor * 2) % size)];

          float t = (x % zoom_factor) / (float)zoom_factor;

          DWORD result = dwCubicInterpolate(y0, y1, y2, y3, t);

          dst[y * dwords_per_line + x] = result;
        }

    // next go down, doing cubic interp along Y, on every line.
    for (int x = 0; x < size; x++)
      for (int y = 0; y < size; y++)
        if (y % zoom_factor) {
          int base_y = (y / zoom_factor) * zoom_factor + size;
          DWORD y0 = dst[((base_y - zoom_factor) % size) * dwords_per_line + x];
          DWORD y1 = dst[((base_y) % size) * dwords_per_line + x];
          DWORD y2 = dst[((base_y + zoom_factor) % size) * dwords_per_line + x];
          DWORD y3 = dst[((base_y + zoom_factor * 2) % size) * dwords_per_line + x];

          float t = (y % zoom_factor) / (float)zoom_factor;

          DWORD result = dwCubicInterpolate(y0, y1, y2, y3, t);

          dst[y * dwords_per_line + x] = result;
        }

  }

  // unlock texture
  pNoiseTex->UnlockRect(0);

  // add it to m_textures[].
  TexInfo x;
  lstrcpyW(x.texname, szTexName);
  x.texptr = pNoiseTex;
  //x.texsize_param = NULL;
  x.w = size;
  x.h = size;
  x.d = 1;
  x.bEvictable = false;
  x.nAge = m_nPresetsLoadedTotal;
  x.nSizeInBytes = 0;
  m_textures.push_back(x);

  return true;
}

bool Engine::AddNoiseVol(const wchar_t* szTexName, int size, int zoom_factor) {
  if (!GetDevice()) {
    // DX12 path: generate 3D noise into CPU buffer, then upload via DX12
    int RANGE = (zoom_factor > 1) ? 216 : 256;
    int dwords_per_line  = size;
    int dwords_per_slice = size * size;
    std::vector<DWORD> pixels(size * size * size);
    DWORD* dst = pixels.data();

    // Generate random noise
    for (int z = 0; z < size; z++) {
      for (int y = 0; y < size; y++) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        srand(q.LowPart ^ q.HighPart ^ rand());
        DWORD* line = dst + z * dwords_per_slice + y * dwords_per_line;
        for (int x = 0; x < size; x++) {
          line[x] = (((DWORD)(rand() % RANGE) + RANGE / 2) << 24) |
                    (((DWORD)(rand() % RANGE) + RANGE / 2) << 16) |
                    (((DWORD)(rand() % RANGE) + RANGE / 2) <<  8) |
                    (((DWORD)(rand() % RANGE) + RANGE / 2));
        }
        // swap some pixels randomly, to improve 'randomness'
        for (int x = 0; x < size; x++) {
          int x1 = (rand() ^ q.LowPart)  % size;
          int x2 = (rand() ^ q.HighPart) % size;
          DWORD temp = line[x2];
          line[x2] = line[x1];
          line[x1] = temp;
        }
      }
    }

    // cubic interpolation smoothing (3-pass: X, Y, Z)
    if (zoom_factor > 1) {
      // Pass 1: cubic interp along X, on main grid lines only
      for (int z = 0; z < size; z += zoom_factor)
        for (int y = 0; y < size; y += zoom_factor)
          for (int x = 0; x < size; x++)
            if (x % zoom_factor) {
              int base_x = (x / zoom_factor) * zoom_factor + size;
              int base_y = z * dwords_per_slice + y * dwords_per_line;
              DWORD y0 = dst[base_y + ((base_x - zoom_factor) % size)];
              DWORD y1 = dst[base_y + ((base_x)               % size)];
              DWORD y2 = dst[base_y + ((base_x + zoom_factor)     % size)];
              DWORD y3 = dst[base_y + ((base_x + zoom_factor * 2) % size)];
              float t = (x % zoom_factor) / (float)zoom_factor;
              dst[z * dwords_per_slice + y * dwords_per_line + x] = dwCubicInterpolate(y0, y1, y2, y3, t);
            }

      // Pass 2: cubic interp along Y, on main slices
      for (int z = 0; z < size; z += zoom_factor)
        for (int x = 0; x < size; x++)
          for (int y = 0; y < size; y++)
            if (y % zoom_factor) {
              int base_y = (y / zoom_factor) * zoom_factor + size;
              int base_z = z * dwords_per_slice;
              DWORD y0 = dst[((base_y - zoom_factor)     % size) * dwords_per_line + base_z + x];
              DWORD y1 = dst[((base_y)                   % size) * dwords_per_line + base_z + x];
              DWORD y2 = dst[((base_y + zoom_factor)     % size) * dwords_per_line + base_z + x];
              DWORD y3 = dst[((base_y + zoom_factor * 2) % size) * dwords_per_line + base_z + x];
              float t = (y % zoom_factor) / (float)zoom_factor;
              dst[y * dwords_per_line + base_z + x] = dwCubicInterpolate(y0, y1, y2, y3, t);
            }

      // Pass 3: cubic interp along Z, everywhere
      for (int x = 0; x < size; x++)
        for (int y = 0; y < size; y++)
          for (int z = 0; z < size; z++)
            if (z % zoom_factor) {
              int base_y = y * dwords_per_line;
              int base_z = (z / zoom_factor) * zoom_factor + size;
              DWORD y0 = dst[((base_z - zoom_factor)     % size) * dwords_per_slice + base_y + x];
              DWORD y1 = dst[((base_z)                   % size) * dwords_per_slice + base_y + x];
              DWORD y2 = dst[((base_z + zoom_factor)     % size) * dwords_per_slice + base_y + x];
              DWORD y3 = dst[((base_z + zoom_factor * 2) % size) * dwords_per_slice + base_y + x];
              float t = (z % zoom_factor) / (float)zoom_factor;
              dst[z * dwords_per_slice + base_y + x] = dwCubicInterpolate(y0, y1, y2, y3, t);
            }
    }

    // Upload to GPU — D3DFMT_A8R8G8B8 byte layout matches DXGI_FORMAT_B8G8R8A8_UNORM
    TexInfo ti;
    lstrcpyW(ti.texname, szTexName);
    ti.texptr = NULL;
    ti.w = size;
    ti.h = size;
    ti.d = size;
    ti.bEvictable = false;
    ti.nAge = m_nPresetsLoadedTotal;
    ti.nSizeInBytes = size * size * size * 4;
    ti.dx12Tex = m_lpDX->CreateVolumeTextureFromPixels(
        pixels.data(), size, size, size,
        size * sizeof(DWORD),
        DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!ti.dx12Tex.IsValid()) {
      dumpmsg(L"DX12: Could not create 3D noise volume texture");
    }
    m_textures.push_back(ti);
    return true;
  }
  // size = width & height & depth of the texture;
  // zoom_factor = how zoomed-in the texture features should be.
  //           1 = random noise
  //           2 = smoothed (interp)
  //           4/8/16... = cubic interp.

  wchar_t buf[2048], title[64];

  // Synthesize noise texture(s)
  LPDIRECT3DVOLUMETEXTURE9 pNoiseTex = NULL;
  // try twice - once with mips, once without.
  // NO, TRY JUST ONCE - DX9 doesn't do auto mipgen w/volume textures.  (Debug runtime complains.)
  for (int i = 1; i < 2; i++) {
    if (D3D_OK != GetDevice()->CreateVolumeTexture(size, size, size, i, D3DUSAGE_DYNAMIC | (i ? 0 : D3DUSAGE_AUTOGENMIPMAP), D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pNoiseTex, NULL)) {
      if (i == 1) {
        wasabiApiLangString(IDS_COULD_NOT_CREATE_3D_NOISE_TEXTURE, buf, sizeof(buf));
        dumpmsg(buf);
        MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
        return false;
      }
    }
    else
      break;
  }
  D3DLOCKED_BOX r;
  if (D3D_OK != pNoiseTex->LockBox(0, &r, NULL, D3DLOCK_DISCARD)) {
    wasabiApiLangString(IDS_COULD_NOT_LOCK_3D_NOISE_TEXTURE, buf, sizeof(buf));
    dumpmsg(buf);
    MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }
  if (r.RowPitch < size * 4 || r.SlicePitch < size * size * 4) {
    wasabiApiLangString(IDS_3D_NOISE_TEXTURE_BYTE_LAYOUT_NOT_RECOGNISED, buf, sizeof(buf));
    dumpmsg(buf);
    MessageBoxW(GetPluginWindow(), buf, wasabiApiLangString(IDS_MILKDROP_ERROR, title, 64), MB_OK | MB_SETFOREGROUND | MB_TOPMOST);
    return false;
  }
  // write to the bits...
  int dwords_per_slice = r.SlicePitch / sizeof(DWORD);
  int dwords_per_line = r.RowPitch / sizeof(DWORD);
  int RANGE = (zoom_factor > 1) ? 216 : 256;
  for (int z = 0; z < size; z++) {
    DWORD* dst = (DWORD*)r.pBits + z * dwords_per_slice;
    for (int y = 0; y < size; y++) {
      LARGE_INTEGER q;
      QueryPerformanceCounter(&q);
      srand(q.LowPart ^ q.HighPart ^ rand());
      for (int x = 0; x < size; x++) {
        dst[x] = (((DWORD)(rand() % RANGE) + RANGE / 2) << 24) |
          (((DWORD)(rand() % RANGE) + RANGE / 2) << 16) |
          (((DWORD)(rand() % RANGE) + RANGE / 2) << 8) |
          (((DWORD)(rand() % RANGE) + RANGE / 2));
      }
      // swap some pixels randomly, to improve 'randomness'
      for (int x = 0; x < size; x++) {
        int x1 = (rand() ^ q.LowPart) % size;
        int x2 = (rand() ^ q.HighPart) % size;
        DWORD temp = dst[x2];
        dst[x2] = dst[x1];
        dst[x1] = temp;
      }
      dst += dwords_per_line;
    }
  }

  // smoothing
  if (zoom_factor > 1) {
    // first go ACROSS, blending cubically on X, but only on the main lines.
    DWORD* dst = (DWORD*)r.pBits;
    for (int z = 0; z < size; z += zoom_factor)
      for (int y = 0; y < size; y += zoom_factor)
        for (int x = 0; x < size; x++)
          if (x % zoom_factor) {
            int base_x = (x / zoom_factor) * zoom_factor + size;
            int base_y = z * dwords_per_slice + y * dwords_per_line;
            DWORD y0 = dst[base_y + ((base_x - zoom_factor) % size)];
            DWORD y1 = dst[base_y + ((base_x) % size)];
            DWORD y2 = dst[base_y + ((base_x + zoom_factor) % size)];
            DWORD y3 = dst[base_y + ((base_x + zoom_factor * 2) % size)];

            float t = (x % zoom_factor) / (float)zoom_factor;

            DWORD result = dwCubicInterpolate(y0, y1, y2, y3, t);

            dst[z * dwords_per_slice + y * dwords_per_line + x] = result;
          }

    // next go down, doing cubic interp along Y, on the main slices.
    for (int z = 0; z < size; z += zoom_factor)
      for (int x = 0; x < size; x++)
        for (int y = 0; y < size; y++)
          if (y % zoom_factor) {
            int base_y = (y / zoom_factor) * zoom_factor + size;
            int base_z = z * dwords_per_slice;
            DWORD y0 = dst[((base_y - zoom_factor) % size) * dwords_per_line + base_z + x];
            DWORD y1 = dst[((base_y) % size) * dwords_per_line + base_z + x];
            DWORD y2 = dst[((base_y + zoom_factor) % size) * dwords_per_line + base_z + x];
            DWORD y3 = dst[((base_y + zoom_factor * 2) % size) * dwords_per_line + base_z + x];

            float t = (y % zoom_factor) / (float)zoom_factor;

            DWORD result = dwCubicInterpolate(y0, y1, y2, y3, t);

            dst[y * dwords_per_line + base_z + x] = result;
          }

    // next go through, doing cubic interp along Z, everywhere.
    for (int x = 0; x < size; x++)
      for (int y = 0; y < size; y++)
        for (int z = 0; z < size; z++)
          if (z % zoom_factor) {
            int base_y = y * dwords_per_line;
            int base_z = (z / zoom_factor) * zoom_factor + size;
            DWORD y0 = dst[((base_z - zoom_factor) % size) * dwords_per_slice + base_y + x];
            DWORD y1 = dst[((base_z) % size) * dwords_per_slice + base_y + x];
            DWORD y2 = dst[((base_z + zoom_factor) % size) * dwords_per_slice + base_y + x];
            DWORD y3 = dst[((base_z + zoom_factor * 2) % size) * dwords_per_slice + base_y + x];

            float t = (z % zoom_factor) / (float)zoom_factor;

            DWORD result = dwCubicInterpolate(y0, y1, y2, y3, t);

            dst[z * dwords_per_slice + base_y + x] = result;
          }

  }

  // unlock texture
  pNoiseTex->UnlockBox(0);

  // add it to m_textures[].
  TexInfo x;
  lstrcpyW(x.texname, szTexName);
  x.texptr = pNoiseTex;
  //x.texsize_param = NULL;
  x.w = size;
  x.h = size;
  x.d = size;
  x.bEvictable = false;
  x.nAge = m_nPresetsLoadedTotal;
  x.nSizeInBytes = 0;
  m_textures.push_back(x);

  return true;
}


bool Engine::EvictSomeTexture() {
  // note: this won't evict a texture whose age is zero,
  //       or whose reported size is zero!

#if _DEBUG
  {
    int nEvictableFiles = 0;
    int nEvictableBytes = 0;
    int N = m_textures.size();
    for (int i = 0; i < N; i++)
      if (m_textures[i].bEvictable && m_textures[i].texptr) {
        nEvictableFiles++;
        nEvictableBytes += m_textures[i].nSizeInBytes;
      }
    char buf[1024];
    sprintf(buf, "evicting at %d textures, %.1f MB\n", nEvictableFiles, nEvictableBytes * 0.000001f);
    DebugLogA(buf);
  }
#endif

  int N = m_textures.size();

  // find age gap
  int newest = 99999999;
  int oldest = 0;
  bool bAtLeastOneFound = false;
  for (int i = 0; i < N; i++)
    if (m_textures[i].bEvictable && m_textures[i].nSizeInBytes > 0 && m_textures[i].nAge < m_nPresetsLoadedTotal - 1) // note: -1 here keeps images around for the blend-from preset, too...
    {
      newest = min(newest, m_textures[i].nAge);
      oldest = max(oldest, m_textures[i].nAge);
      bAtLeastOneFound = true;
    }
  if (!bAtLeastOneFound)
    return false;

  // find the "biggest" texture, but dilate things so that the newest textures
  // are HALF as big as the oldest textures, and thus, less likely to get booted.
  int biggest_bytes = 0;
  int biggest_index = -1;
  for (int i = 0; i < N; i++)
    if (m_textures[i].bEvictable && m_textures[i].nSizeInBytes > 0 && m_textures[i].nAge < m_nPresetsLoadedTotal - 1) // note: -1 here keeps images around for the blend-from preset, too...
    {
      float size_mult = 1.0f + (m_textures[i].nAge - newest) / (float)(oldest - newest);
      int bytes = (int)(m_textures[i].nSizeInBytes * size_mult);
      if (bytes > biggest_bytes) {
        biggest_bytes = bytes;
        biggest_index = i;
      }
    }
  if (biggest_index == -1)
    return false;


  // evict that sucker
  assert(m_textures[biggest_index].texptr);

  // notify all CShaderParams classes that we're releasing a bindable texture!!
  N = global_CShaderParams_master_list.size();
  for (int i = 0; i < N; i++)
    global_CShaderParams_master_list[i]->OnTextureEvict(m_textures[biggest_index].texptr);

  // 2. erase the texture itself
  SafeRelease(m_textures[biggest_index].texptr);
  m_textures.erase(m_textures.begin() + biggest_index);

  return true;
}

std::wstring texture_exts[] = { L"jpg", L"jpeg", L"jfif", L"dds", L"png", L"tga", L"bmp", L"dib" };
extern const int texture_exts_count = _countof(texture_exts);
extern const wchar_t szExtsWithSlashes[] = L".jpg|.png|.dds|etc.";
typedef std::vector<std::wstring> StringVec;
bool PickRandomTexture(const wchar_t* prefix, wchar_t* szRetTextureFilename)  //should be MAX_PATH chars
{
  static StringVec texfiles;
  static DWORD     texfiles_timestamp = 0;   // update this a max of every ~2 seconds or so

  // if it's been more than a few seconds since the last textures dir scan, redo it.
  // (..just enough to make sure we don't do it more than once per preset load)
  //DWORD t = timeGetTime(); // in milliseconds
  //if (abs(t - texfiles_timestamp) > 2000)
  if (g_engine.m_bNeedRescanTexturesDir) {
    g_engine.m_bNeedRescanTexturesDir = false;
    texfiles.clear();

    // Helper lambda: scan a directory for valid texture files (filename only, no path)
    auto scanDir = [](const wchar_t* szDir, StringVec& out) {
      wchar_t szMask[MAX_PATH];
      swprintf(szMask, L"%s*.*", szDir);
      WIN32_FIND_DATAW ffd = { 0 };
      HANDLE hFind = FindFirstFileW(szMask, &ffd);
      if (hFind == INVALID_HANDLE_VALUE) return;
      do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t* ext = wcsrchr(ffd.cFileName, L'.');
        if (!ext) continue;
        for (int i = 0; i < texture_exts_count; i++)
          if (!wcsicmp(texture_exts[i].c_str(), ext + 1)) {
            out.push_back(ffd.cFileName);
            break;
          }
      } while (FindNextFileW(hFind, &ffd));
      FindClose(hFind);
    };

    // 1) Dedicated random textures directory (highest priority)
    if (g_engine.m_szRandomTexDir[0])
      scanDir(g_engine.m_szRandomTexDir, texfiles);

    // 2) Fallback paths (user's texture collection)
    if (texfiles.empty()) {
      for (auto& fbPath : g_engine.m_fallbackPaths)
        scanDir(fbPath.c_str(), texfiles);
    }

    // 3) Built-in textures directory
    if (texfiles.empty()) {
      wchar_t szBuiltin[MAX_PATH];
      swprintf(szBuiltin, L"%stextures\\", g_engine.m_szMilkdrop2Path);
      scanDir(szBuiltin, texfiles);
    }
  }

  if (texfiles.size() == 0)
    return false;

  // Use high-resolution timer for a well-distributed random pick
  // (MSVC rand() is only 15-bit and srand() isn't called before preset loads)
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  unsigned int rng = (unsigned int)(qpc.LowPart ^ (qpc.HighPart * 2654435761u));

  // then randomly pick one
  if (prefix == NULL || prefix[0] == 0) {
    // pick randomly from entire list
    int i = rng % texfiles.size();
    lstrcpyW(szRetTextureFilename, texfiles[i].c_str());
  }
  else {
    // only pick from files w/the right prefix
    StringVec temp_list;
    int N = texfiles.size();
    int len = lstrlenW(prefix);
    for (int i = 0; i < N; i++)
      if (!_wcsnicmp(prefix, texfiles[i].c_str(), len))
        temp_list.push_back(texfiles[i]);
    N = temp_list.size();
    if (N == 0)
      return false;
    // pick randomly from the subset
    int i = rng % temp_list.size();
    lstrcpyW(szRetTextureFilename, temp_list[i].c_str());
  }
  return true;
}


} // namespace mdrop
