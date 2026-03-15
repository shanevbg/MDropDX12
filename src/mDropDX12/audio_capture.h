// audio_capture.h — WASAPI loopback audio capture for MDropDX12
// Consolidated from src/audio/ (audiobuf.h, loopback-capture.h, cleanup.h, common.h, prefs.h)

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mutex>
#include <string>

// ---------------------------------------------------------------------------
// RAII cleanup helpers (from cleanup.h, ERR() replaced with OutputDebugStringW)
// ---------------------------------------------------------------------------

class AudioClientStopOnExit {
public:
  AudioClientStopOnExit(IAudioClient* p) : m_p(p) {}
  ~AudioClientStopOnExit() {
    HRESULT hr = m_p->Stop();
    if (FAILED(hr)) {
      wchar_t buf[128];
      swprintf_s(buf, L"IAudioClient::Stop failed: hr = 0x%08x\n", hr);
      OutputDebugStringW(buf);
    }
  }
private:
  IAudioClient* m_p;
};

class AvRevertMmThreadCharacteristicsOnExit {
public:
  AvRevertMmThreadCharacteristicsOnExit(HANDLE hTask) : m_hTask(hTask) {}
  ~AvRevertMmThreadCharacteristicsOnExit() {
    if (!AvRevertMmThreadCharacteristics(m_hTask)) {
      wchar_t buf[128];
      swprintf_s(buf, L"AvRevertMmThreadCharacteristics failed: last error is %d\n", GetLastError());
      OutputDebugStringW(buf);
    }
  }
private:
  HANDLE m_hTask;
};

class CancelWaitableTimerOnExit {
public:
  CancelWaitableTimerOnExit(HANDLE h) : m_h(h) {}
  ~CancelWaitableTimerOnExit() {
    if (!CancelWaitableTimer(m_h)) {
      wchar_t buf[128];
      swprintf_s(buf, L"CancelWaitableTimer failed: last error is %d\n", GetLastError());
      OutputDebugStringW(buf);
    }
  }
private:
  HANDLE m_h;
};

class CloseHandleOnExit {
public:
  CloseHandleOnExit(HANDLE h) : m_h(h) {}
  ~CloseHandleOnExit() {
    if (!CloseHandle(m_h)) {
      wchar_t buf[128];
      swprintf_s(buf, L"CloseHandle failed: last error is %d\n", GetLastError());
      OutputDebugStringW(buf);
    }
  }
private:
  HANDLE m_h;
};

class CoTaskMemFreeOnExit {
public:
  CoTaskMemFreeOnExit(PVOID p) : m_p(p) {}
  ~CoTaskMemFreeOnExit() {
    CoTaskMemFree(m_p);
  }
private:
  PVOID m_p;
};

class CoUninitializeOnExit {
public:
  ~CoUninitializeOnExit() {
    CoUninitialize();
  }
};

class PropVariantClearOnExit {
public:
  PropVariantClearOnExit(PROPVARIANT* p) : m_p(p) {}
  ~PropVariantClearOnExit() {
    HRESULT hr = PropVariantClear(m_p);
    if (FAILED(hr)) {
      wchar_t buf[128];
      swprintf_s(buf, L"PropVariantClear failed: hr = 0x%08x\n", hr);
      OutputDebugStringW(buf);
    }
  }
private:
  PROPVARIANT* m_p;
};

class ReleaseOnExit {
public:
  ReleaseOnExit(IUnknown* p) : m_p(p) {}
  ~ReleaseOnExit() {
    m_p->Release();
  }
private:
  IUnknown* m_p;
};

class SetEventOnExit {
public:
  SetEventOnExit(HANDLE h) : m_h(h) {}
  ~SetEventOnExit() {
    if (!SetEvent(m_h)) {
      wchar_t buf[128];
      swprintf_s(buf, L"SetEvent failed: last error is %d\n", GetLastError());
      OutputDebugStringW(buf);
    }
  }
private:
  HANDLE m_h;
};

class WaitForSingleObjectOnExit {
public:
  WaitForSingleObjectOnExit(HANDLE h) : m_h(h) {}
  ~WaitForSingleObjectOnExit() {
    DWORD dwWaitResult = WaitForSingleObject(m_h, INFINITE);
    if (WAIT_OBJECT_0 != dwWaitResult) {
      wchar_t buf[128];
      swprintf_s(buf, L"WaitForSingleObject returned unexpected result 0x%08x, last error is %d\n", dwWaitResult, GetLastError());
      OutputDebugStringW(buf);
    }
  }
private:
  HANDLE m_h;
};

// ---------------------------------------------------------------------------
// Audio buffer (from audiobuf.h)
// ---------------------------------------------------------------------------

#define TARGET_SAMPLE_RATE 44100

void ResetAudioBuf();
void GetAudioBuf(unsigned char* pWaveL, unsigned char* pWaveR, int SamplesCount);
void GetAudioBufFloat(float* pWaveL, float* pWaveR, int SamplesCount);
void SetAudioBuf(const BYTE* pData, const UINT32 nNumFramesToRead, const WAVEFORMATEX* pwfx, const bool bInt16);

// External audio gain/sensitivity controls (defined in audio_capture.cpp)
extern float mdropdx12_amp_left;
extern float mdropdx12_amp_right;
extern float mdropdx12_audio_sensitivity;  // fixed gain multiplier (default 1.0 = passthrough)

// ---------------------------------------------------------------------------
// Device resolution (replaces CPrefs from prefs.h)
// ---------------------------------------------------------------------------

HRESULT GetDefaultLoopbackDevice(IMMDevice** ppMMDevice, std::wstring& displayName);
HRESULT GetSpecificAudioDevice(LPCWSTR szName, IMMDevice** ppMMDevice, bool bRenderDevices, std::wstring& displayName);

// ---------------------------------------------------------------------------
// Device volume control (IAudioEndpointVolume)
// ---------------------------------------------------------------------------

HRESULT GetDeviceVolume(LPCWSTR szDeviceName, int nDeviceType, float* pVolume, BOOL* pMuted);
HRESULT SetDeviceVolume(LPCWSTR szDeviceName, int nDeviceType, float volume);
HRESULT SetDeviceMute(LPCWSTR szDeviceName, int nDeviceType, BOOL muted);

// ---------------------------------------------------------------------------
// Loopback capture thread (from loopback-capture.h)
// ---------------------------------------------------------------------------

class MDropDX12; // forward declaration

struct LoopbackCaptureThreadFunctionArguments {
  IMMDevice* pMMDevice;
  bool bIsRenderDevice;
  bool bInt16;
  HANDLE hStartedEvent;
  HANDLE hStopEvent;
  UINT32 nFrames;
  HRESULT hr;
  MDropDX12* pMDropDX12;
};

DWORD WINAPI LoopbackCaptureThreadFunction(LPVOID pContext);
