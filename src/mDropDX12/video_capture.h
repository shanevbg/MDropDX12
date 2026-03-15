// video_capture.h — Media Foundation webcam / video file capture for MDropDX12
//
// Captures frames from a webcam or video file using IMFSourceReader,
// delivers them as BGRA pixel buffers for GPU upload on the render thread.

#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include <string>
#include <mutex>

#include "dx12helpers.h"

using Microsoft::WRL::ComPtr;

class DXContext;  // forward declaration (global namespace)

namespace mdrop {

// ---------------------------------------------------------------------------
// VideoCaptureSource — webcam or video file frame source
// ---------------------------------------------------------------------------
class VideoCaptureSource {
public:
    enum SourceType { None, Webcam, VideoFile };

    VideoCaptureSource();
    ~VideoCaptureSource();

    // Lifecycle
    bool OpenWebcam(const wchar_t* szDeviceName);   // empty = default device
    bool OpenVideoFile(const wchar_t* szFilePath);
    void Close();

    // Per-frame: call from render thread to upload latest frame to GPU
    // Returns true if a new frame was uploaded this call.
    bool UpdateTexture(::DXContext* dx);

    // State
    bool IsConnected() const { return m_bConnected.load(); }
    UINT GetWidth()  const { return m_nWidth; }
    UINT GetHeight() const { return m_nHeight; }
    SourceType GetType() const { return m_type; }

    // Device enumeration: returns list of {friendlyName, symbolicLink} pairs
    struct DeviceInfo { std::wstring name; std::wstring id; };
    static std::vector<DeviceInfo> EnumerateWebcams();

    // Video file looping
    bool m_bLoop = true;

    // GPU texture for rendering (valid after first successful UpdateTexture)
    DX12Texture m_dx12Tex;

private:
    // Shared state
    SourceType m_type = None;
    std::atomic<bool> m_bConnected{false};
    std::atomic<bool> m_bNewFrame{false};
    UINT m_nWidth = 0;
    UINT m_nHeight = 0;

    // Frame buffer (written by capture thread, read by render thread)
    // Always stored as top-down, width*4 stride BGRA by ExtractFrameData()
    std::mutex m_frameMutex;
    std::vector<BYTE> m_frameBuffer;
    UINT m_nFrameWidth = 0;   // dimensions of data in m_frameBuffer
    UINT m_nFrameHeight = 0;

    // Media Foundation format info
    LONG m_nStride = 0;         // actual row stride (negative = bottom-up)
    bool m_bMFStarted = false;  // tracks MFStartup/MFShutdown pairing

    // GPU upload resources (owned by render thread)
    ComPtr<ID3D12Resource> m_pUploadBuffer;
    ComPtr<ID3D12Resource> m_pGpuTexture;
    UINT m_nGpuTexWidth = 0;
    UINT m_nGpuTexHeight = 0;

    // Capture thread
    HANDLE m_hCaptureThread = nullptr;
    std::atomic<bool> m_bStopCapture{false};

    // Media Foundation source reader (owned by capture thread)
    ComPtr<IMFSourceReader> m_pReader;

    // Thread entry points
    static unsigned __stdcall CaptureThreadProc(void* pArg);
    void CaptureLoop();
    void VideoFileLoop();

    // Frame extraction: copies MF sample to m_frameBuffer (top-down, w*4 stride)
    bool ExtractFrameData(IMFSample* pSample);

    // GPU resource management (render thread only)
    bool EnsureGpuTexture(::DXContext* dx, UINT w, UINT h);
    bool UploadFrameToGpu(::DXContext* dx, const BYTE* pPixels, UINT w, UINT h);
};

} // namespace mdrop
