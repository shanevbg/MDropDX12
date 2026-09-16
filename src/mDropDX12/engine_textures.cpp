/*
  Plugin module: Texture Management
  Extracted from engine.cpp for maintainability.
  Contains: AddNoiseTex, AddNoiseVol, EvictSomeTexture, PickRandomTexture
*/

#include "engine.h"
#include "utility.h"
#include "format_to.h"
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

// Forward declarations for helpers defined in engine.cpp
DWORD dwCubicInterpolate(DWORD y0, DWORD y1, DWORD y2, DWORD y3, float t);
float fCubicInterpolate(float y0, float y1, float y2, float y3, float t);

bool Engine::AddNoiseTex(const wchar_t* szTexName, int size, int zoom_factor) {
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
    CopyTo(x.texname, szTexName);
    x.texptr = NULL;
    x.w = size;
    x.h = size;
    x.d = 1;
    x.bEvictable = false;
    x.nAge = m_nPresetsLoadedTotal;
    x.dx12Tex = m_lpDX->CreateTextureFromPixels(pixels.data(), size, size,
                                                 size * sizeof(DWORD),
                                                 DXGI_FORMAT_B8G8R8A8_UNORM);
    // AFTER the resource exists, and from the device (#347) -- size*size*4 is
    // the base level only, and these carry a mip chain like any other texture.
    x.nSizeInBytes = (int)m_lpDX->TextureVramBytes(x.dx12Tex);
    m_textures.push_back(x);
    PublishTextureCacheStats();   // #352
    return true;
  // The DX9 branch that stood here is gone (issue 17): there was never a DX9
  // device under DX12, so the block above always runs and always returns. The
  // if/else wrapper went with it.
}

bool Engine::AddNoiseVol(const wchar_t* szTexName, int size, int zoom_factor) {
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
    CopyTo(ti.texname, szTexName);
    ti.texptr = NULL;
    ti.w = size;
    ti.h = size;
    ti.d = size;
    ti.bEvictable = false;
    ti.nAge = m_nPresetsLoadedTotal;
    ti.dx12Tex = m_lpDX->CreateVolumeTextureFromPixels(
        pixels.data(), size, size, size,
        size * sizeof(DWORD),
        DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!ti.dx12Tex.IsValid()) {
      dumpmsg(L"DX12: Could not create 3D noise volume texture");
    }
    // From the device, after the resource exists (#347).
    ti.nSizeInBytes = (int)m_lpDX->TextureVramBytes(ti.dx12Tex);
    m_textures.push_back(ti);
    PublishTextureCacheStats();   // #352
    return true;
  // The DX9 branch that stood here is gone (issue 17): there was never a DX9
  // device under DX12, so the block above always runs and always returns. The
  // if/else wrapper went with it.
}

// ── Shadertoy-compatible noise textures ──────────────────────────────────
// Shadertoy noise textures are: uniform white noise, full [0,255] range,
// NO cubic interpolation, fixed seed (deterministic). This ensures shaders
// that depend on specific noise characteristics (like iq's "Selfie Girl")
// get functionally equivalent noise instead of MilkDrop's smoothed variants.

bool Engine::AddNoiseTex_ST(const wchar_t* szTexName, int size) {

  // Use a deterministic seed based on the texture name for reproducibility
  unsigned int seed = 0;
  for (const wchar_t* p = szTexName; *p; p++)
    seed = seed * 31 + *p;
  srand(seed);

  std::vector<DWORD> pixels(size * size);
  for (int i = 0; i < size * size; i++) {
    BYTE r = (BYTE)(rand() & 0xFF);
    BYTE g = (BYTE)(rand() & 0xFF);
    BYTE b = (BYTE)(rand() & 0xFF);
    BYTE a = (BYTE)(rand() & 0xFF);
    pixels[i] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
  }

  TexInfo x;
  CopyTo(x.texname, szTexName);
  x.texptr = NULL;
  x.w = size;
  x.h = size;
  x.d = 1;
  x.bEvictable = false;
  x.nAge = m_nPresetsLoadedTotal;
  x.dx12Tex = m_lpDX->CreateTextureFromPixels(pixels.data(), size, size,
                                               size * sizeof(DWORD),
                                               DXGI_FORMAT_B8G8R8A8_UNORM);
  // From the device, after the resource exists (#347).
  x.nSizeInBytes = (int)m_lpDX->TextureVramBytes(x.dx12Tex);
  m_textures.push_back(x);
  PublishTextureCacheStats();   // #352
  return true;
}

bool Engine::AddNoiseVol_ST(const wchar_t* szTexName, int size) {

  unsigned int seed = 0;
  for (const wchar_t* p = szTexName; *p; p++)
    seed = seed * 31 + *p;
  srand(seed);

  std::vector<DWORD> pixels(size * size * size);
  for (int i = 0; i < size * size * size; i++) {
    BYTE r = (BYTE)(rand() & 0xFF);
    BYTE g = (BYTE)(rand() & 0xFF);
    BYTE b = (BYTE)(rand() & 0xFF);
    BYTE a = (BYTE)(rand() & 0xFF);
    pixels[i] = ((DWORD)a << 24) | ((DWORD)r << 16) | ((DWORD)g << 8) | b;
  }

  TexInfo ti;
  CopyTo(ti.texname, szTexName);
  ti.texptr = NULL;
  ti.w = size;
  ti.h = size;
  ti.d = size;
  ti.bEvictable = false;
  ti.nAge = m_nPresetsLoadedTotal;
  ti.dx12Tex = m_lpDX->CreateVolumeTextureFromPixels(
      pixels.data(), size, size, size,
      size * sizeof(DWORD),
      DXGI_FORMAT_B8G8R8A8_UNORM);
  // From the device, after the resource exists (#347).
  ti.nSizeInBytes = (int)m_lpDX->TextureVramBytes(ti.dx12Tex);
  m_textures.push_back(ti);
  PublishTextureCacheStats();   // #352
  return true;
}

// Evict until the texture cache is inside its configured caps (#312).
//
// EvictSomeTexture below has always existed and NOTHING EVER CALLED IT. Both
// caps -- MaxBytes and MaxImages -- were read from settings.ini at startup and
// written back out at shutdown, and never compared against anything in between.
// So the cache grew for the life of the process.
//
// Measured on the harness before this: 50 presets from one library took the
// cache from 2 MB to 3,228 MB, with 3,226 MB of that marked evictable, against
// a configured MaxBytes of 2,000 MB. Process VRAM went 446 MB -> 4,948 MB. A
// 1,270-preset sweep reached 13.4 GB and the low-VRAM guard shut the app down,
// which is the only reason this presented as a clean exit rather than a
// device-removed crash.
//
// At roughly 70 MB a distinct preset, an ordinary unattended session on a
// 30-second cycle exhausts a 15 GB card in under two hours.
//
// Is this texture's descriptor named by anything that is about to draw? (#312)
//
// The age filter EvictSomeTexture uses -- nAge < m_nPresetsLoadedTotal - 1 --
// is a proxy, and a weak one: "two presets ago" says nothing about what a
// shader still holds. A .milk2 keeps both of its presets resident, a mirror
// surface runs its OWN preset on its own schedule, and the fallback shaders are
// substituted into live sets. Any of those can hold a binding to a texture the
// counter calls old.
//
// So ask the bindings instead. Every set that could be bound next frame is
// walked and its SRV indices collected; a texture whose index is in that set is
// never evicted, whatever its age.
bool Engine::TextureIsBound(const TexInfo& t) const {
  const UINT srv = t.dx12Tex.srvIndex;
  if (srv == UINT_MAX) return false;

  auto bound = [srv](const CShaderParams& p) {
    for (int i = 0; i < 32; i++)
      if (p.m_texture_bindings[i].dx12SrvIndex == srv) return true;
    return false;
  };
  auto boundSet = [&](const PShaderSet& s) {
    return bound(s.warp.params) || bound(s.comp.params) ||
           bound(s.bufferA.params) || bound(s.bufferB.params) ||
           bound(s.bufferC.params) || bound(s.bufferD.params);
  };

  if (boundSet(m_shaders) || boundSet(m_OldShaders)) return true;
  // The .milk2 second preset, which is co-resident rather than transitioning.
  if (boundSet(m_Milk2OldShaders)) return true;
  // Every mirror surface running its own preset. These are on their own
  // timeline entirely, so preset age cannot speak for them at all.
  for (const auto& surf : m_mirrorSurfaces)
    if (surf && boundSet(surf->sim.shaders) && true) return true;
  return false;
}

// Evict until the texture cache fits MaxBytes/MaxImages (#312).
//
// Both caps were read from settings.ini at startup and written back at
// shutdown and compared against nothing in between, and EvictSomeTexture had no
// callers at all, so the cache grew for the life of the process: 3.3 GB after
// fifty presets against a configured 2 GB, and a 1,270-preset sweep reached
// 13.4 GB of process VRAM before the low-VRAM guard closed the app.
//
// THE DRAIN IS NOT OPTIONAL. Calling eviction without it kills the device
// inside a dozen presets -- "DRED: page fault VA = 0x0", device hung -- because
// a command list already submitted is still sampling the texture being freed.
// This is the same WaitForGpu-plus-engine-lock that LoadPresetTick performs
// before it releases an outgoing preset's resources, and for the identical
// reason.
//
// Cheap in practice despite that: the loop returns immediately unless the cache
// is actually over budget, so the wait is paid only on the loads that evict,
// not on every texture.
// What the cache is holding, for anyone who is not the render thread (#352).
//
// The vector may only be WALKED by the thread that owns it, so the three
// numbers DIAG_GPU wants are published here instead, at every point the cache
// changes. Cheap: it is one pass over a container the caller has just finished
// walking anyway, and it happens on preset loads, not per frame.
void Engine::PublishTextureCacheStats() {
  int count = 0;
  long long bytes = 0, evictable = 0;
  for (const auto& t : m_textures) {
    count++;
    bytes += (long long)t.nSizeInBytes;
    if (t.bEvictable) evictable += (long long)t.nSizeInBytes;
  }
  m_texCacheCount.store(count, std::memory_order_relaxed);
  m_texCacheBytes.store(bytes, std::memory_order_relaxed);
  m_texCacheEvictableBytes.store(evictable, std::memory_order_relaxed);
}

void Engine::EnforceTextureCacheBudget() {
  // Nothing to do is the common case -- check before paying for a drain.
  auto overBudget = [this]() {
    long long bytes = 0;
    int count = 0;
    for (const auto& t : m_textures) {
      if (t.nSizeInBytes <= 0) continue;
      bytes += t.nSizeInBytes;
      count++;
    }
    return (m_nMaxBytes > 0 && bytes > (long long)m_nMaxBytes) ||
           (m_nMaxImages > 0 && count > m_nMaxImages);
  };
  // Published on BOTH exits. This is the only path that grows the cache with a
  // preset's own images and the only caller of EvictSomeTexture, so it is the
  // one place that sees every change worth reporting (#352).
  if (!overBudget()) { PublishTextureCacheStats(); return; }

  // Drain, then hold the mirror workers off while the vector is mutated.
  if (m_lpDX) m_lpDX->WaitForGpu();
  else        WaitForSurfaceWork();
  LockMirrorEngine();
  struct Unlocker {
    Engine* e;
    ~Unlocker() { e->UnlockMirrorEngine(); }
  } unlock{ this };

  // Bounded rather than while(true): EvictSomeTexture returning false already
  // ends it, and a cap set absurdly low would otherwise spin against a cache
  // made entirely of textures that are bound and may not be dropped.
  int evicted = 0;
  for (int guard = 0; guard < 8192 && overBudget(); guard++) {
    if (!EvictSomeTexture()) break;   // nothing left that may be dropped
    evicted++;
  }
  if (evicted)
    DLOG_INFO("texture cache: evicted %d texture(s) to stay inside "
              "MaxBytes=%d / MaxImages=%d", evicted, m_nMaxBytes, m_nMaxImages);
  PublishTextureCacheStats();
}

bool Engine::EvictSomeTexture() {
  // note: this won't evict a texture whose age is zero,
  //       or whose reported size is zero!

#if _DEBUG
  {
    int nEvictableFiles = 0;
    long long nEvictableBytes = 0;   // a texture cache can exceed 2 GB
    const size_t N = m_textures.size();
    for (size_t i = 0; i < N; i++)
      if (m_textures[i].bEvictable && m_textures[i].texptr) {
        nEvictableFiles++;
        nEvictableBytes += m_textures[i].nSizeInBytes;
      }
    char buf[1024];
    sprintf(buf, "evicting at %d textures, %.1f MB\n", nEvictableFiles, nEvictableBytes * 0.000001f);
    DebugLogA(buf, LOG_VERBOSE);
  }
#endif

  int N = (int)m_textures.size();

  // find age gap
  int newest = 99999999;
  int oldest = 0;
  bool bAtLeastOneFound = false;
  for (int i = 0; i < N; i++)
    if (m_textures[i].bEvictable && m_textures[i].nSizeInBytes > 0 &&
        m_textures[i].nAge < m_nPresetsLoadedTotal - 1 &&   // keeps the blend-from preset's images too
        !TextureIsBound(m_textures[i]))                     // ...and anything still named by a live binding (#312)
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
    if (m_textures[i].bEvictable && m_textures[i].nSizeInBytes > 0 &&
        m_textures[i].nAge < m_nPresetsLoadedTotal - 1 &&   // keeps the blend-from preset's images too
        !TextureIsBound(m_textures[i]))                     // ...and anything still named by a live binding (#312)
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
  //
  // The assert(texptr) that used to stand here was a DX9 leftover and it was
  // ALWAYS false on this backend: texptr is the legacy D3D9 pointer and is null
  // for every texture the DX12 loader creates. In a release build it did
  // nothing, which is how a release path that frees nothing survived.
  //
  // The CShaderParams eviction-notify that used to be here walked a provably
  // empty list and is gone (issue 17). What keeps an in-use texture safe is the
  // pair of filters above -- age AND TextureIsBound -- plus the drain the
  // caller performs before entering here (#312).

  // Release the DX12 resource and hand the descriptor slots back. THIS is where
  // the memory actually returns: SafeRelease(texptr) below frees nothing at
  // all, because there is nothing on the other end of that pointer.
  if (m_lpDX)
    m_lpDX->RecycleTexture(m_textures[biggest_index].dx12Tex);

  SafeRelease(m_textures[biggest_index].texptr);  // harmless no-op under DX12
  m_textures.erase(m_textures.begin() + biggest_index);

  return true;
}

std::wstring texture_exts[] = { L"jpg", L"jpeg", L"jfif", L"dds", L"png", L"tga", L"bmp", L"dib" };
extern const int texture_exts_count = _countof(texture_exts);
extern const wchar_t szExtsWithSlashes[] = L".jpg|.png|.dds|etc.";
typedef std::vector<std::wstring> StringVec;
bool PickRandomTexture(const wchar_t* prefix, wchar_t* szRetTextureFilename)  //should be MAX_PATH chars
{
  // Cached one list PER SOURCE, in priority order, rather than as a single
  // flattened list (#322).
  //
  // The scan cannot know the prefix -- it happens once per preset load, and the
  // prefix arrives per texture. Flattening with an early-out therefore let a
  // source that merely HAS textures shut out the source that has the RIGHT
  // ones: with any file in the random-textures directory, `rand00_smalltiled`
  // never reached the user's collection where the smalltiled_* files live, and
  // reported "[rand00] smalltiled*" as a missing texture.
  //
  // Keeping the sources separate preserves the priority the early-out was for,
  // and defers "is this source any use" to the point where the prefix is known.
  static std::vector<StringVec> texsets;

  if (g_engine.m_bNeedRescanTexturesDir) {
    g_engine.m_bNeedRescanTexturesDir = false;
    texsets.clear();

    // Helper lambda: scan a directory for valid texture files (filename only, no path)
    auto scanDir = [](const wchar_t* szDir, StringVec& out) {
      wchar_t szMask[MAX_PATH];
      FormatTo(szMask, L"%s*.*", szDir);
      WIN32_FIND_DATAW ffd = { 0 };
      HANDLE hFind = FindFirstFileW(szMask, &ffd);
      if (hFind == INVALID_HANDLE_VALUE) return;
      do {
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t* ext = wcsrchr(ffd.cFileName, L'.');
        if (!ext) continue;
        for (int i = 0; i < texture_exts_count; i++)
          if (!_wcsicmp(texture_exts[i].c_str(), ext + 1)) {
            out.push_back(ffd.cFileName);
            break;
          }
      } while (FindNextFileW(hFind, &ffd));
      FindClose(hFind);
    };

    // Every source is scanned now; which one supplies the texture is decided
    // below, once the prefix is known. An empty source is simply dropped.
    auto addSource = [&](StringVec&& v) {
      if (!v.empty()) texsets.push_back(std::move(v));
    };

    // 1) Dedicated random textures directory (highest priority)
    if (g_engine.m_szRandomTexDir[0]) {
      StringVec v;
      scanDir(g_engine.m_szRandomTexDir, v);
      addSource(std::move(v));
    }

    // 2) Content base path
    if (g_engine.m_szContentBasePath[0]) {
      StringVec v;
      scanDir(g_engine.m_szContentBasePath, v);
      addSource(std::move(v));
    }

    // 3) Fallback paths (user's texture collection) -- one source between them,
    // because they are a single collection spread over several directories and
    // none of them outranks another.
    {
      StringVec v;
      // A copy: scanDir walks a whole directory per entry.
      for (auto& fbPath : g_engine.FallbackPathsCopy())
        scanDir(fbPath.c_str(), v);
      addSource(std::move(v));
    }

    // 4) Built-in textures directory
    {
      wchar_t szBuiltin[MAX_PATH];
      FormatTo(szBuiltin, L"%stextures\\", g_engine.m_szMilkdrop2Path);
      StringVec v;
      scanDir(szBuiltin, v);
      addSource(std::move(v));
    }
  }

  if (texsets.empty())
    return false;

  // Use high-resolution timer for a well-distributed random pick
  // (MSVC rand() is only 15-bit and srand() isn't called before preset loads)
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  unsigned int rng = (unsigned int)(qpc.LowPart ^ (qpc.HighPart * 2654435761u));

  // Walk the sources in priority order and take the first that can actually
  // satisfy the request. Unprefixed, that is the first source at all, which is
  // exactly what the old early-out produced; prefixed, it is the first source
  // holding a matching name, which is what the old code could not express.
  const int len = prefix ? lstrlenW(prefix) : 0;
  for (const StringVec& set : texsets) {
    if (len == 0) {
      lstrcpynW(szRetTextureFilename, set[rng % set.size()].c_str(), MAX_PATH);
      return true;
    }
    StringVec matches;
    for (const std::wstring& f : set)
      if (!_wcsnicmp(prefix, f.c_str(), len))
        matches.push_back(f);
    if (!matches.empty()) {
      lstrcpynW(szRetTextureFilename, matches[rng % matches.size()].c_str(), MAX_PATH);
      return true;
    }
  }
  return false;
}

} // namespace mdrop
