// audio_capture.cpp — WASAPI loopback audio capture for MDropDX12
// Consolidated from src/audio/ (audiobuf.cpp, loopback-capture.cpp, prefs.cpp, guid.cpp, log.cpp)

#include <initguid.h>      // MUST be first — instantiates WASAPI GUIDs (replaces guid.cpp)
#include "audio_capture.h"
#include "d3dx9compat.h"    // DebugLogA, DebugLogW, LOG_VERBOSE, LOG_ERROR, LOG_INFO
#include "utility.h"        // DLOG_* macros
#include "mdropdx12.h"      // MDropDX12 class for logging from capture thread

#include <stdexcept>
#include <vector>

// Helper: format + OutputDebugStringW (for contexts where DebugLogW is not safe)
static void AudioErr(LPCWSTR format, ...) {
  wchar_t buf[512];
  va_list args;
  va_start(args, format);
  vswprintf_s(buf, format, args);
  va_end(args);
  OutputDebugStringW(buf);
}

// ===========================================================================
// Circular audio buffer (from audiobuf.cpp)
// ===========================================================================

#define SAMPLE_SIZE_LPB 576

std::mutex pcmLpbMutex;
unsigned char pcmLeftLpb[SAMPLE_SIZE_LPB];
unsigned char pcmRightLpb[SAMPLE_SIZE_LPB];
float pcmLeftFloatLpb[SAMPLE_SIZE_LPB];
float pcmRightFloatLpb[SAMPLE_SIZE_LPB];
bool pcmBufDrained = false;
signed int pcmLen = 0;
signed int pcmPos = 0;

float mdropdx12_amp_left = 1.0f;
float mdropdx12_amp_right = 1.0f;
float mdropdx12_audio_sensitivity = 1.0f;

void ResetAudioBuf() {
  std::unique_lock<std::mutex> lock(pcmLpbMutex);
  memset(pcmLeftLpb, 128, SAMPLE_SIZE_LPB);
  memset(pcmRightLpb, 128, SAMPLE_SIZE_LPB);
  memset(pcmLeftFloatLpb, 0, sizeof(pcmLeftFloatLpb));
  memset(pcmRightFloatLpb, 0, sizeof(pcmRightFloatLpb));
  pcmBufDrained = false;
  pcmLen = 0;
}

void GetAudioBuf(unsigned char* pWaveL, unsigned char* pWaveR, int SamplesCount) {
  std::unique_lock<std::mutex> lock(pcmLpbMutex);
  static int consecutiveReads = 0;
  static int lastPcmPos = pcmPos;

  if (pcmPos == lastPcmPos) {
    consecutiveReads++;
  }
  else {
    consecutiveReads = 0;
    lastPcmPos = pcmPos;
  }

  if ((pcmLen < SamplesCount) || (consecutiveReads > 3)) {
    memset(pWaveL, 128, SamplesCount);
    memset(pWaveR, 128, SamplesCount);
    if (consecutiveReads > 3)
      pcmBufDrained = true;
  }
  else {
    for (int i = 0; i < SamplesCount; i++) {
      pWaveL[i % SamplesCount] = pcmLeftLpb[(pcmPos + i) % SAMPLE_SIZE_LPB];
      pWaveR[i % SamplesCount] = pcmRightLpb[(pcmPos + i) % SAMPLE_SIZE_LPB];
    }
  }
}

void GetAudioBufFloat(float* pWaveL, float* pWaveR, int SamplesCount) {
  std::unique_lock<std::mutex> lock(pcmLpbMutex);
  static int consecutiveReads = 0;
  static int lastPcmPos = pcmPos;

  if (pcmPos == lastPcmPos) {
    consecutiveReads++;
  }
  else {
    consecutiveReads = 0;
    lastPcmPos = pcmPos;
  }

  if ((pcmLen < SamplesCount) || (consecutiveReads > 3)) {
    memset(pWaveL, 0, SamplesCount * sizeof(float));
    memset(pWaveR, 0, SamplesCount * sizeof(float));
    if (consecutiveReads > 3)
      pcmBufDrained = true;
  }
  else {
    for (int i = 0; i < SamplesCount; i++) {
      // Match the legacy waveform amplitude domain used by the FFT path:
      // old m_sound.fWaveform samples were roughly in [-128..127].
      pWaveL[i % SamplesCount] = pcmLeftFloatLpb[(pcmPos + i) % SAMPLE_SIZE_LPB] * 128.0f;
      pWaveR[i % SamplesCount] = pcmRightFloatLpb[(pcmPos + i) % SAMPLE_SIZE_LPB] * 128.0f;
    }
  }
}

// Float-to-int8 conversion — matches Milkwave's audiobuf.cpp.
// WASAPI float [-1..+1] → int8 [-128..+127], straight passthrough.
// Gain (mdropdx12_audio_sensitivity) is applied in SetAudioBuf, not here.
int8_t FltToInt(float flt) {
  if (flt >= 1.0f)  return +127;
  if (flt < -1.0f)  return -128;
  return (int8_t)(flt * 128);
}

union u_type {
  int32_t IntVar;
  float FltVar;
  uint8_t Bytes[4];
};

int8_t GetChannelSample(const BYTE* pData, int BlockOffset, int ChannelOffset, const bool bInt16) {
  u_type sample;

  sample.IntVar = 0;
  sample.Bytes[0] = pData[BlockOffset + ChannelOffset + 0];
  sample.Bytes[1] = pData[BlockOffset + ChannelOffset + 1];
  if (!bInt16) {
    sample.Bytes[2] = pData[BlockOffset + ChannelOffset + 2];
    sample.Bytes[3] = pData[BlockOffset + ChannelOffset + 3];
  }

  if (!bInt16) {
    return FltToInt(sample.FltVar);
  }
  else {
    return (signed char)(sample.IntVar / 256);
  }
}

// Float version — returns [-1..+1] with full precision (matches Milkwave's GetChannelSampleFloat)
static float GetChannelSampleFloat(const BYTE* pData, int BlockOffset, int ChannelOffset, const bool bInt16) {
  u_type sample;
  sample.IntVar = 0;
  sample.Bytes[0] = pData[BlockOffset + ChannelOffset + 0];
  sample.Bytes[1] = pData[BlockOffset + ChannelOffset + 1];
  if (!bInt16) {
    sample.Bytes[2] = pData[BlockOffset + ChannelOffset + 2];
    sample.Bytes[3] = pData[BlockOffset + ChannelOffset + 3];
    if (sample.FltVar >= 1.0f) return 1.0f;
    if (sample.FltVar <= -1.0f) return -1.0f;
    return sample.FltVar;
  }
  float v = (float)((int16_t)sample.IntVar) / 32768.0f;
  if (v >= 1.0f) return 1.0f;
  if (v <= -1.0f) return -1.0f;
  return v;
}

// Matches Milkwave's SetAudioBuf — sums as floats, converts to uint8 at the end.
// Previous MDropDX12 code converted to int8 first (losing precision), then summed integers.
void SetAudioBuf(const BYTE* pData, const UINT32 nNumFramesToRead, const WAVEFORMATEX* pwfx, const bool bInt16) {
  std::unique_lock<std::mutex> lock(pcmLpbMutex);

  int downsampleRatio = 1;
  if (pwfx->nSamplesPerSec > TARGET_SAMPLE_RATE) {
    downsampleRatio = pwfx->nSamplesPerSec / TARGET_SAMPLE_RATE;
  }

  int outputSamples = nNumFramesToRead / downsampleRatio;

  int n = 0;
  int start = 0;
  int len = outputSamples;
  if (outputSamples >= SAMPLE_SIZE_LPB) {
    n = 0;
    start = outputSamples - SAMPLE_SIZE_LPB;
    len = SAMPLE_SIZE_LPB;
  }
  else {
    n = SAMPLE_SIZE_LPB - outputSamples;
    start = 0;
    len = outputSamples;
  }

  for (int i = start; i < len; i++, n++) {
    float sumLeft = 0.0f;
    float sumRight = 0.0f;

    for (int j = 0; j < downsampleRatio; j++) {
      int inputIndex = i * downsampleRatio + j;
      if (inputIndex >= (int)nNumFramesToRead) break;

      int blockOffset = inputIndex * pwfx->nBlockAlign;

      float sampleLeft = 0.0f;
      if (pData && pwfx->nChannels >= 1) {
        sampleLeft = GetChannelSampleFloat(pData, blockOffset, 0, bInt16);
      }
      sumLeft += sampleLeft;

      float sampleRight = sampleLeft;
      if (pData && pwfx->nChannels >= 2) {
        sampleRight = GetChannelSampleFloat(pData, blockOffset, pwfx->wBitsPerSample / 8, bInt16);
      }
      sumRight += sampleRight;
    }

    float avgLeft = sumLeft / downsampleRatio * mdropdx12_audio_sensitivity * mdropdx12_amp_left;
    float avgRight = sumRight / downsampleRatio * mdropdx12_audio_sensitivity * mdropdx12_amp_right;
    if (avgLeft > 1.0f) avgLeft = 1.0f;
    if (avgLeft < -1.0f) avgLeft = -1.0f;
    if (avgRight > 1.0f) avgRight = 1.0f;
    if (avgRight < -1.0f) avgRight = -1.0f;
    pcmLeftFloatLpb[(pcmPos + n) % SAMPLE_SIZE_LPB] = avgLeft;
    pcmRightFloatLpb[(pcmPos + n) % SAMPLE_SIZE_LPB] = avgRight;
    pcmLeftLpb[(pcmPos + n) % SAMPLE_SIZE_LPB] = (uint8_t)(FltToInt(avgLeft) + 128);
    pcmRightLpb[(pcmPos + n) % SAMPLE_SIZE_LPB] = (uint8_t)(FltToInt(avgRight) + 128);
  }

  pcmBufDrained = false;
  pcmLen = (pcmLen + len <= SAMPLE_SIZE_LPB) ? (pcmLen + len) : (SAMPLE_SIZE_LPB);
  pcmPos = (pcmPos + len) % SAMPLE_SIZE_LPB;
}

// ===========================================================================
// Device resolution (from prefs.cpp, simplified — no CPrefs, no WAV file)
// ===========================================================================

HRESULT GetDefaultLoopbackDevice(IMMDevice** ppMMDevice, std::wstring& displayName) {
  HRESULT hr = S_OK;
  IMMDeviceEnumerator* pMMDeviceEnumerator;

  hr = CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator),
    (void**)&pMMDeviceEnumerator
  );
  if (FAILED(hr)) {
    AudioErr(L"CoCreateInstance(IMMDeviceEnumerator) failed: hr = 0x%08x", hr);
    return hr;
  }
  ReleaseOnExit releaseMMDeviceEnumerator(pMMDeviceEnumerator);

  hr = pMMDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, ppMMDevice);
  if (FAILED(hr)) {
    AudioErr(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint failed: hr = 0x%08x", hr);
    return hr;
  }

  IPropertyStore* pPropertyStore;
  hr = (*ppMMDevice)->OpenPropertyStore(STGM_READ, &pPropertyStore);
  if (FAILED(hr)) {
    AudioErr(L"IMMDevice::OpenPropertyStore failed: hr = 0x%08x", hr);
    return hr;
  }
  ReleaseOnExit releasePropertyStore(pPropertyStore);

  PROPVARIANT pv; PropVariantInit(&pv);
  hr = pPropertyStore->GetValue(PKEY_Device_FriendlyName, &pv);
  if (FAILED(hr)) {
    AudioErr(L"IPropertyStore::GetValue failed: hr = 0x%08x", hr);
    return hr;
  }
  PropVariantClearOnExit clearPv(&pv);

  if (VT_LPWSTR != pv.vt) {
    AudioErr(L"PKEY_Device_FriendlyName variant type is %u - expected VT_LPWSTR", pv.vt);
    return E_UNEXPECTED;
  }
  displayName = std::wstring(pv.pwszVal);

  return S_OK;
}

HRESULT GetSpecificAudioDevice(LPCWSTR szLongName, IMMDevice** ppMMDevice, bool bRenderDevices, std::wstring& displayName) {
  HRESULT hr = S_OK;

  *ppMMDevice = NULL;

  IMMDeviceEnumerator* pMMDeviceEnumerator;
  hr = CoCreateInstance(
    __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
    __uuidof(IMMDeviceEnumerator),
    (void**)&pMMDeviceEnumerator
  );
  if (FAILED(hr)) {
    AudioErr(L"CoCreateInstance(IMMDeviceEnumerator) failed: hr = 0x%08x", hr);
    return hr;
  }
  ReleaseOnExit releaseMMDeviceEnumerator(pMMDeviceEnumerator);

  IMMDeviceCollection* pMMDeviceCollection;
  hr = pMMDeviceEnumerator->EnumAudioEndpoints(
    bRenderDevices ? eRender : eCapture,
    DEVICE_STATE_ACTIVE, &pMMDeviceCollection
  );
  if (FAILED(hr)) {
    AudioErr(L"IMMDeviceEnumerator::EnumAudioEndpoints failed: hr = 0x%08x", hr);
    return hr;
  }
  ReleaseOnExit releaseMMDeviceCollection(pMMDeviceCollection);

  UINT count;
  hr = pMMDeviceCollection->GetCount(&count);
  if (FAILED(hr)) {
    AudioErr(L"IMMDeviceCollection::GetCount failed: hr = 0x%08x", hr);
    return hr;
  }

  for (UINT i = 0; i < count; i++) {
    IMMDevice* pMMDevice;
    hr = pMMDeviceCollection->Item(i, &pMMDevice);
    if (FAILED(hr)) {
      AudioErr(L"IMMDeviceCollection::Item failed: hr = 0x%08x", hr);
      return hr;
    }
    ReleaseOnExit releaseMMDevice(pMMDevice);

    IPropertyStore* pPropertyStore;
    hr = pMMDevice->OpenPropertyStore(STGM_READ, &pPropertyStore);
    if (FAILED(hr)) {
      AudioErr(L"IMMDevice::OpenPropertyStore failed: hr = 0x%08x", hr);
      return hr;
    }
    ReleaseOnExit releasePropertyStore(pPropertyStore);

    PROPVARIANT pv; PropVariantInit(&pv);
    hr = pPropertyStore->GetValue(PKEY_Device_FriendlyName, &pv);
    if (FAILED(hr)) {
      AudioErr(L"IPropertyStore::GetValue failed: hr = 0x%08x", hr);
      return hr;
    }
    PropVariantClearOnExit clearPv(&pv);

    if (VT_LPWSTR != pv.vt) {
      AudioErr(L"PKEY_Device_FriendlyName variant type is %u - expected VT_LPWSTR", pv.vt);
      return E_UNEXPECTED;
    }

    if (0 == _wcsicmp(pv.pwszVal, szLongName)) {
      if (NULL == *ppMMDevice) {
        *ppMMDevice = pMMDevice;
        pMMDevice->AddRef();

        if (bRenderDevices) {
          displayName = std::wstring(szLongName) + L" [Out]";
        }
        else {
          displayName = std::wstring(szLongName) + L" [In]";
        }
      }
      else {
        AudioErr(L"Found (at least) two devices named %ls", szLongName);
        return E_UNEXPECTED;
      }
    }
  }

  if (NULL == *ppMMDevice) {
    AudioErr(L"Could not find a device named %ls", szLongName);
    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  }

  return S_OK;
}

// ===========================================================================
// WASAPI loopback capture (from loopback-capture.cpp, WAV recording stripped)
// ===========================================================================

// Forward declarations
static HRESULT GetBufferWithRetry(
  IAudioCaptureClient* pAudioCaptureClient,
  BYTE** ppData,
  UINT32* pNumFramesToRead,
  DWORD* pFlags,
  MDropDX12* pMDropDX12
);

static HRESULT LoopbackCapture(
  IMMDevice* pMMDevice,
  bool bIsRenderDevice,
  bool bInt16,
  MDropDX12* pMDropDX12,
  HANDLE hStartedEvent,
  HANDLE hStopEvent,
  PUINT32 pnFrames
);

DWORD WINAPI LoopbackCaptureThreadFunction(LPVOID pContext) {
  LoopbackCaptureThreadFunctionArguments* pArgs =
    (LoopbackCaptureThreadFunctionArguments*)pContext;

  if (pArgs && pArgs->pMDropDX12) {
    try {
      pArgs->pMDropDX12->LogInfo(L"LoopbackCaptureThreadFunction: thread starting");
    } catch (...) {}
  }

  pArgs->hr = CoInitialize(NULL);
  if (FAILED(pArgs->hr)) {
    if (pArgs && pArgs->pMDropDX12) {
      wchar_t buf[128];
      swprintf_s(buf, L"CoInitialize failed: hr = 0x%08x", pArgs->hr);
      try { pArgs->pMDropDX12->LogInfo(buf); } catch (...) {}
    }
    return 0;
  }
  CoUninitializeOnExit cuoe;

  try {
    pArgs->hr = LoopbackCapture(
      pArgs->pMMDevice,
      pArgs->bIsRenderDevice,
      pArgs->bInt16,
      pArgs->pMDropDX12,
      pArgs->hStartedEvent,
      pArgs->hStopEvent,
      &pArgs->nFrames
    );
  } catch (const std::exception& e) {
    if (pArgs && pArgs->pMDropDX12) {
      try {
        pArgs->pMDropDX12->LogException(L"LoopbackCaptureThreadFunction: exception", e, true);
      } catch (...) {}
    }
    pArgs->hr = E_FAIL;
  } catch (...) {
    if (pArgs && pArgs->pMDropDX12) {
      try {
        try {
          throw;
        } catch (const std::exception& e) {
          pArgs->pMDropDX12->LogException(L"LoopbackCaptureThreadFunction: rethrown std::exception", e, true);
        } catch (const wchar_t* ws) {
          std::string msg;
          if (ws) {
            int size = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
            if (size > 0) {
              msg.resize(size - 1);
              WideCharToMultiByte(CP_UTF8, 0, ws, -1, &msg[0], size, nullptr, nullptr);
            }
          }
          else { msg = "(null)"; }
          std::runtime_error re(msg);
          pArgs->pMDropDX12->LogException(L"LoopbackCaptureThreadFunction: non-std exception (wchar_t*)", re, true);
        } catch (const char* cs) {
          std::runtime_error re(cs ? cs : "(null)");
          pArgs->pMDropDX12->LogException(L"LoopbackCaptureThreadFunction: non-std exception (char*)", re, true);
        } catch (...) {
          std::runtime_error re("Unknown non-std exception");
          pArgs->pMDropDX12->LogException(L"LoopbackCaptureThreadFunction: unknown non-std exception", re, true);
        }
      } catch (...) {}
    }
    pArgs->hr = E_FAIL;
  }

  return 0;
}

static HRESULT LoopbackCapture(
  IMMDevice* pMMDevice,
  bool bIsRenderDevice,
  bool bInt16,
  MDropDX12* pMDropDX12,
  HANDLE hStartedEvent,
  HANDLE hStopEvent,
  PUINT32 pnFrames
) {
  HRESULT hr;

  if (pMDropDX12) {
    try { pMDropDX12->LogInfo(L"LoopbackCapture: starting capture"); } catch (...) {}
  }

  if (pMDropDX12 && pMMDevice) {
    try {
      LPWSTR deviceId = nullptr;
      HRESULT hrDeviceId = pMMDevice->GetId(&deviceId);
      if (SUCCEEDED(hrDeviceId)) {
        CoTaskMemFreeOnExit freeId(deviceId);
        wchar_t buf[512];
        swprintf_s(buf, L"LoopbackCapture: deviceId=%ls isRender=%ls", deviceId, bIsRenderDevice ? L"true" : L"false");
        pMDropDX12->LogDebug(buf);
      }
      else {
        wchar_t buf[256];
        swprintf_s(buf, L"LoopbackCapture: GetId failed hr=0x%08x", hrDeviceId);
        pMDropDX12->LogDebug(buf);
      }
    } catch (...) {}
  }

  try {
    // activate an IAudioClient
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IMMDevice::Activate"); } catch (...) {} }
    IAudioClient* pAudioClient;
    hr = pMMDevice->Activate(
      __uuidof(IAudioClient),
      CLSCTX_ALL, NULL,
      (void**)&pAudioClient
    );
    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IMMDevice::Activate failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }
    ReleaseOnExit releaseAudioClient(pAudioClient);

    // get the default device periodicity
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IAudioClient::GetDevicePeriod"); } catch (...) {} }
    REFERENCE_TIME hnsDefaultDevicePeriod;
    hr = pAudioClient->GetDevicePeriod(&hnsDefaultDevicePeriod, NULL);
    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioClient::GetDevicePeriod failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }

    if (pMDropDX12) {
      try {
        double defaultPeriodMs = static_cast<double>(hnsDefaultDevicePeriod) / 10000.0;
        wchar_t buf[256];
        swprintf_s(buf, L"LoopbackCapture: default period %.3f ms", defaultPeriodMs);
        pMDropDX12->LogDebug(buf);
      } catch (...) {}
    }

    // get the default device format
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IAudioClient::GetMixFormat"); } catch (...) {} }
    WAVEFORMATEX* pwfx;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioClient::GetMixFormat failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }
    CoTaskMemFreeOnExit freeMixFormat(pwfx);

    if (pMDropDX12 && pwfx) {
      try {
        wchar_t buf[256];
        swprintf_s(
          buf,
          L"LoopbackCapture: mix format rate=%lu channels=%u bits=%u blockAlign=%u avgBytesPerSec=%lu",
          pwfx->nSamplesPerSec,
          pwfx->nChannels,
          pwfx->wBitsPerSample,
          pwfx->nBlockAlign,
          pwfx->nAvgBytesPerSec
        );
        pMDropDX12->LogDebug(buf);
      } catch (...) {}
    }

    if (bInt16) {
      // coerce int-16 wave format
      switch (pwfx->wFormatTag) {
      case WAVE_FORMAT_IEEE_FLOAT:
        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->wBitsPerSample = 16;
        pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
        pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
        break;

      case WAVE_FORMAT_EXTENSIBLE:
      {
        PWAVEFORMATEXTENSIBLE pEx = reinterpret_cast<PWAVEFORMATEXTENSIBLE>(pwfx);
        if (IsEqualGUID(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, pEx->SubFormat)) {
          pEx->SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
          pEx->Samples.wValidBitsPerSample = 16;
          pwfx->wBitsPerSample = 16;
          pwfx->nBlockAlign = pwfx->nChannels * pwfx->wBitsPerSample / 8;
          pwfx->nAvgBytesPerSec = pwfx->nBlockAlign * pwfx->nSamplesPerSec;
        }
        else {
          if (pMDropDX12) { try { pMDropDX12->LogInfo(L"Don't know how to coerce mix format to int-16"); } catch (...) {} }
          return E_UNEXPECTED;
        }
      }
      break;

      default:
        if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"Don't know how to coerce WAVEFORMATEX with wFormatTag = 0x%08x to int-16", pwfx->wFormatTag); pMDropDX12->LogInfo(buf); } catch (...) {} }
        return E_UNEXPECTED;
      }
    }

    // create a periodic waitable timer
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: creating waitable timer"); } catch (...) {} }
    HANDLE hWakeUp = CreateWaitableTimer(NULL, FALSE, NULL);
    if (NULL == hWakeUp) {
      DWORD dwErr = GetLastError();
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"CreateWaitableTimer failed: last error = %u", dwErr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return HRESULT_FROM_WIN32(dwErr);
    }
    CloseHandleOnExit closeWakeUp(hWakeUp);

    UINT32 nBlockAlign = pwfx->nBlockAlign;
    *pnFrames = 0;

    // call IAudioClient::Initialize
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IAudioClient::Initialize"); } catch (...) {} }
    hr = pAudioClient->Initialize(
      AUDCLNT_SHAREMODE_SHARED,
      bIsRenderDevice ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
      0, 0, pwfx, nullptr
    );

    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioClient::Initialize failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }

    if (pMDropDX12) {
      try {
        wchar_t buf[256];
        swprintf_s(
          buf,
          L"LoopbackCapture: initialize flags loopback=%ls int16=%ls",
          bIsRenderDevice ? L"true" : L"false",
          bInt16 ? L"true" : L"false"
        );
        pMDropDX12->LogDebug(buf);
      } catch (...) {}
    }

    // activate an IAudioCaptureClient
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IAudioClient::GetService for IAudioCaptureClient"); } catch (...) {} }
    IAudioCaptureClient* pAudioCaptureClient;
    hr = pAudioClient->GetService(
      __uuidof(IAudioCaptureClient),
      (void**)&pAudioCaptureClient
    );
    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioClient::GetService(IAudioCaptureClient) failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }
    ReleaseOnExit releaseAudioCaptureClient(pAudioCaptureClient);

    // set the waitable timer
    LARGE_INTEGER liFirstFire;
    liFirstFire.QuadPart = -hnsDefaultDevicePeriod / 2;
    LONG lTimeBetweenFires = (LONG)hnsDefaultDevicePeriod / 2 / (10 * 1000);
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling SetWaitableTimer"); } catch (...) {} }
    BOOL bOK = SetWaitableTimer(
      hWakeUp,
      &liFirstFire,
      lTimeBetweenFires,
      NULL, NULL, FALSE
    );
    if (!bOK) {
      DWORD dwErr = GetLastError();
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"SetWaitableTimer failed: last error = %u", dwErr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return HRESULT_FROM_WIN32(dwErr);
    }
    CancelWaitableTimerOnExit cancelWakeUp(hWakeUp);

    // call IAudioClient::Start
    if (pMDropDX12) { try { pMDropDX12->LogInfo(L"LoopbackCapture: calling IAudioClient::Start"); } catch (...) {} }
    hr = pAudioClient->Start();
    if (FAILED(hr)) {
      if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioClient::Start failed: hr = 0x%08x", hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
      return hr;
    }
    AudioClientStopOnExit stopAudioClient(pAudioClient);

    SetEvent(hStartedEvent);

    // loopback capture loop
    HANDLE waitArray[2] = { hStopEvent, hWakeUp };
    DWORD dwWaitResult;

    bool bDone = false;
    bool bFirstPacket = true;
    bool bErrorInAudioData = false;

    for (UINT32 nPasses = 0; !bDone; nPasses++) {
      UINT32 nNextPacketSize;
      for (
        hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize);
        SUCCEEDED(hr) && nNextPacketSize > 0;
        hr = pAudioCaptureClient->GetNextPacketSize(&nNextPacketSize)
        ) {
        BYTE* pData = nullptr;
        UINT32 nNumFramesToRead = 0;
        DWORD dwFlags = 0;

        hr = GetBufferWithRetry(pAudioCaptureClient, &pData, &nNumFramesToRead, &dwFlags, pMDropDX12);

        bErrorInAudioData = false;

        if (FAILED(hr)) {
          bErrorInAudioData = true;
          if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioCaptureClient::GetBuffer failed on pass %u after %u frames: hr = 0x%08x", nPasses, *pnFrames, hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
          return hr;
        }

        const bool bSilentBuffer = (dwFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        DWORD dwFlagsWithoutSilent = dwFlags & ~AUDCLNT_BUFFERFLAGS_SILENT;

        if (bFirstPacket && AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY == dwFlagsWithoutSilent) {
          // Probably spurious glitch reported on first packet
        }
        else if (0 != dwFlagsWithoutSilent) {
          bErrorInAudioData = true;
          if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioCaptureClient::GetBuffer set flags to 0x%08x on pass %u after %u frames", dwFlags, nPasses, *pnFrames); pMDropDX12->LogInfo(buf); } catch (...) {} }
        }

        if (0 == nNumFramesToRead) {
          bErrorInAudioData = true;
          if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioCaptureClient::GetBuffer said to read 0 frames on pass %u after %u frames", nPasses, *pnFrames); pMDropDX12->LogInfo(buf); } catch (...) {} }
        }

        if (bErrorInAudioData) {
          ResetAudioBuf();
        }
        else {
          std::vector<BYTE> silentBuffer;
          const BYTE* pDataForVisualizer = pData;
          if (bSilentBuffer) {
            silentBuffer.assign(static_cast<size_t>(nNumFramesToRead) * nBlockAlign, 0);
            pDataForVisualizer = silentBuffer.data();
          }

          SetAudioBuf(pDataForVisualizer, nNumFramesToRead, pwfx, bInt16);
          *pnFrames += nNumFramesToRead;
        }

        hr = pAudioCaptureClient->ReleaseBuffer(nNumFramesToRead);
        if (FAILED(hr)) {
          if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioCaptureClient::ReleaseBuffer failed on pass %u after %u frames: hr = 0x%08x", nPasses, *pnFrames, hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
          return hr;
        }

        bFirstPacket = false;
      }

      if (FAILED(hr)) {
        if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"IAudioCaptureClient::GetNextPacketSize failed on pass %u after %u frames: hr = 0x%08x", nPasses, *pnFrames, hr); pMDropDX12->LogInfo(buf); } catch (...) {} }
        return hr;
      }

      dwWaitResult = WaitForMultipleObjects(
        ARRAYSIZE(waitArray), waitArray,
        FALSE, INFINITE
      );

      if (WAIT_OBJECT_0 == dwWaitResult) {
        if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"Received stop event after %u passes and %u frames", nPasses, *pnFrames); pMDropDX12->LogInfo(buf); } catch (...) {} }
        bDone = true;
        continue;
      }

      if (WAIT_OBJECT_0 + 1 != dwWaitResult) {
        if (pMDropDX12) { try { wchar_t buf[256]; swprintf_s(buf, L"Unexpected WaitForMultipleObjects return value %u on pass %u after %u frames", dwWaitResult, nPasses, *pnFrames); pMDropDX12->LogInfo(buf); } catch (...) {} }
        return E_UNEXPECTED;
      }
    } // capture loop

    return hr;
  } catch (const std::exception& e) {
    if (pMDropDX12) {
      try { pMDropDX12->LogException(L"LoopbackCapture: exception", e, true); } catch (...) {}
    }
    return E_FAIL;
  } catch (...) {
    if (pMDropDX12) {
      try {
        std::runtime_error re("Unknown non-std exception in LoopbackCapture");
        pMDropDX12->LogException(L"LoopbackCapture: unknown exception", re, true);
      } catch (...) {}
    }
    return E_FAIL;
  }
}

// Helper function for GetBuffer with SEH protection and retry logic
// Isolated in its own function to avoid mixing SEH with C++ exception handling
static HRESULT GetBufferWithRetry(
  IAudioCaptureClient* pAudioCaptureClient,
  BYTE** ppData,
  UINT32* pNumFramesToRead,
  DWORD* pFlags,
  MDropDX12* pMDropDX12
) {
  const int kMaxRetries = 5;
  int backoffMs = 100;
  HRESULT hr = S_OK;

#if defined(_WIN32) && defined(_MSC_VER)
  for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
    bool hadSeh = false;
    DWORD sehCode = 0;
    void* vtablePtr = nullptr;
    HRESULT attemptHr = S_OK;
    BYTE* pDataResult = nullptr;
    UINT32 framesResult = 0;
    DWORD flagsResult = 0;

    __try {
      if (nullptr == pAudioCaptureClient) {
        AudioErr(L"GetBufferWithRetry: pAudioCaptureClient is NULL");
        hr = E_POINTER;
        break;
      }

      vtablePtr = (void*)(*((void**)pAudioCaptureClient));

      attemptHr = pAudioCaptureClient->GetBuffer(
        &pDataResult,
        &framesResult,
        &flagsResult,
        NULL,
        NULL
      );
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      hadSeh = true;
      sehCode = GetExceptionCode();
      attemptHr = E_FAIL;
      AudioErr(L"GetBufferWithRetry: SEH exception 0x%08x (attempt %d)", sehCode, attempt + 1);
    }

    if (!hadSeh && SUCCEEDED(attemptHr)) {
      *ppData = pDataResult;
      *pNumFramesToRead = framesResult;
      *pFlags = flagsResult;
      hr = attemptHr;
    } else {
      hr = attemptHr;
    }

    // Log AFTER __try/__except
    if (pMDropDX12) {
      wchar_t buf[256];
      if (hadSeh) {
        swprintf_s(buf, L"GetBufferWithRetry: attempt %d SEH exception 0x%08x (pAudioCaptureClient=%p, vtable=%p)",
          attempt + 1, sehCode, pAudioCaptureClient, vtablePtr);
      } else {
        swprintf_s(buf, L"GetBufferWithRetry: attempt %d hr=0x%08x (pData=%p, frames=%u, flags=0x%08x)",
          attempt + 1, hr, pDataResult, framesResult, flagsResult);
      }
      pMDropDX12->LogInfo(buf);
    }

    if (!hadSeh && SUCCEEDED(hr) && (pDataResult != nullptr || (flagsResult & AUDCLNT_BUFFERFLAGS_SILENT))) {
      break;
    }

    if (attempt == kMaxRetries - 1) {
      AudioErr(L"GetBufferWithRetry: failed after %d attempts, hr=0x%08x", kMaxRetries, hr);
      if (pMDropDX12) {
        wchar_t buf[256];
        swprintf_s(buf, L"GetBufferWithRetry: FAILED after %d attempts, hr=0x%08x", kMaxRetries, hr);
        pMDropDX12->LogInfo(buf);
      }
      break;
    }

    if (pMDropDX12) {
      wchar_t buf[128];
      swprintf_s(buf, L"GetBufferWithRetry: attempt %d failed, backing off %d ms", attempt + 1, backoffMs);
      pMDropDX12->LogDebug(buf);
    }
    Sleep(backoffMs);
    backoffMs = backoffMs * 2;
  }
#else
  if (nullptr == pAudioCaptureClient) {
    AudioErr(L"GetBufferWithRetry: pAudioCaptureClient is NULL");
    return E_POINTER;
  }
  hr = pAudioCaptureClient->GetBuffer(
    ppData,
    pNumFramesToRead,
    pFlags,
    NULL,
    NULL
  );
#endif

  return hr;
}
