// video_capture.cpp — Media Foundation webcam / video file capture
//
// Uses IMFSourceReader for both webcam and video file sources.
// Frames are decoded to MFVideoFormat_RGB32 (BGRA) on a dedicated thread,
// then uploaded to a DX12 texture on the render thread.

#include "video_capture.h"
#include "dxcontext.h"
#include "utility.h"
#include <mferror.h>
#include <process.h>  // _beginthreadex
#include <algorithm>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mf.lib")

namespace mdrop {

// ---------------------------------------------------------------------------
// Construction / Destruction
// ---------------------------------------------------------------------------

VideoCaptureSource::VideoCaptureSource() {}

VideoCaptureSource::~VideoCaptureSource()
{
    Close();
}

// ---------------------------------------------------------------------------
// EnumerateWebcams — list available video capture devices
// ---------------------------------------------------------------------------
std::vector<VideoCaptureSource::DeviceInfo> VideoCaptureSource::EnumerateWebcams()
{
    std::vector<DeviceInfo> devices;

    if (FAILED(MFStartup(MF_VERSION))) return devices;

    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) { MFShutdown(); return devices; }

    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { pAttributes->Release(); MFShutdown(); return devices; }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();
    if (FAILED(hr)) { MFShutdown(); return devices; }

    for (UINT32 i = 0; i < count; i++) {
        DeviceInfo info;

        // Friendly name
        WCHAR* szName = nullptr;
        UINT32 nameLen = 0;
        hr = ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &szName, &nameLen);
        if (SUCCEEDED(hr) && szName) {
            info.name = szName;
            CoTaskMemFree(szName);
        }

        // Symbolic link (unique ID)
        WCHAR* szLink = nullptr;
        UINT32 linkLen = 0;
        hr = ppDevices[i]->GetAllocatedString(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &szLink, &linkLen);
        if (SUCCEEDED(hr) && szLink) {
            info.id = szLink;
            CoTaskMemFree(szLink);
        }

        devices.push_back(std::move(info));
        ppDevices[i]->Release();
    }
    CoTaskMemFree(ppDevices);

    MFShutdown();
    return devices;
}

// ---------------------------------------------------------------------------
// OpenWebcam — activate a video capture device and start reading frames
// ---------------------------------------------------------------------------
bool VideoCaptureSource::OpenWebcam(const wchar_t* szDeviceName)
{
    Close();

    // Ensure Media Foundation is initialized
    if (!m_bMFStarted) {
        if (FAILED(MFStartup(MF_VERSION))) {
            DebugLogA("VideoCapture: MFStartup failed", LOG_ERROR);
            return false;
        }
        m_bMFStarted = true;
    }

    // Find the device by name (or use first available if empty)
    IMFAttributes* pAttributes = nullptr;
    HRESULT hr = MFCreateAttributes(&pAttributes, 1);
    if (FAILED(hr)) return false;

    hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) { pAttributes->Release(); return false; }

    IMFActivate** ppDevices = nullptr;
    UINT32 count = 0;
    hr = MFEnumDeviceSources(pAttributes, &ppDevices, &count);
    pAttributes->Release();
    if (FAILED(hr) || count == 0) {
        DebugLogA("VideoCapture: No webcam devices found", LOG_ERROR);
        return false;
    }

    // Select device: match by name or use first
    IMFActivate* pActivate = nullptr;
    for (UINT32 i = 0; i < count; i++) {
        if (!szDeviceName || szDeviceName[0] == L'\0') {
            pActivate = ppDevices[0];
            break;
        }
        WCHAR* szName = nullptr;
        UINT32 nameLen = 0;
        ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &szName, &nameLen);
        if (szName) {
            bool match = (wcscmp(szName, szDeviceName) == 0);
            CoTaskMemFree(szName);
            if (match) { pActivate = ppDevices[i]; break; }
        }
    }
    if (!pActivate) pActivate = ppDevices[0]; // fallback to first

    // Create media source from the device
    IMFMediaSource* pSource = nullptr;
    hr = pActivate->ActivateObject(__uuidof(IMFMediaSource), (void**)&pSource);

    // Release device list (but NOT pActivate since it points into the array)
    for (UINT32 i = 0; i < count; i++)
        ppDevices[i]->Release();
    CoTaskMemFree(ppDevices);

    if (FAILED(hr)) {
        DebugLogA("VideoCapture: Failed to activate webcam device", LOG_ERROR);
        return false;
    }

    // Create source reader requesting RGB32 (BGRA) output
    IMFAttributes* pReaderAttrs = nullptr;
    MFCreateAttributes(&pReaderAttrs, 1);
    if (pReaderAttrs)
        pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttrs, m_pReader.GetAddressOf());
    pSource->Release();
    if (pReaderAttrs) pReaderAttrs->Release();

    if (FAILED(hr)) {
        DebugLogA("VideoCapture: Failed to create source reader for webcam", LOG_ERROR);
        return false;
    }

    // Configure output format: RGB32 (BGRA)
    ComPtr<IMFMediaType> pType;
    hr = MFCreateMediaType(pType.GetAddressOf());
    if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = m_pReader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType.Get());

    if (FAILED(hr)) {
        DebugLogA("VideoCapture: Failed to set webcam output format to RGB32", LOG_ERROR);
        m_pReader.Reset();
        return false;
    }

    // Read back the actual format to get width/height/stride
    ComPtr<IMFMediaType> pActualType;
    hr = m_pReader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, pActualType.GetAddressOf());
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pActualType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        m_nWidth = w;
        m_nHeight = h;

        // Get actual stride (negative = bottom-up, typical for RGB32)
        LONG stride = 0;
        if (FAILED(pActualType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride)) || stride == 0)
            stride = -(LONG)(w * 4); // RGB32 default: bottom-up
        m_nStride = stride;
    }

    m_type = Webcam;
    m_bStopCapture = false;
    m_bConnected = true;
    m_bNewFrame = false;

    // Start capture thread
    m_hCaptureThread = (HANDLE)_beginthreadex(nullptr, 0, CaptureThreadProc, this, 0, nullptr);
    if (!m_hCaptureThread) {
        DebugLogA("VideoCapture: Failed to create capture thread", LOG_ERROR);
        Close();
        return false;
    }

    DLOG_INFO("VideoCapture: Webcam opened %ux%u", m_nWidth, m_nHeight);
    return true;
}

// ---------------------------------------------------------------------------
// OpenVideoFile — open a video file for frame-by-frame reading
// ---------------------------------------------------------------------------
bool VideoCaptureSource::OpenVideoFile(const wchar_t* szFilePath)
{
    Close();

    if (!szFilePath || szFilePath[0] == L'\0') return false;

    // Ensure Media Foundation is initialized
    if (!m_bMFStarted) {
        if (FAILED(MFStartup(MF_VERSION))) {
            DebugLogA("VideoCapture: MFStartup failed", LOG_ERROR);
            return false;
        }
        m_bMFStarted = true;
    }

    // Create source reader from URL/file path
    IMFAttributes* pReaderAttrs = nullptr;
    MFCreateAttributes(&pReaderAttrs, 2);
    if (pReaderAttrs) {
        pReaderAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        pReaderAttrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    HRESULT hr = MFCreateSourceReaderFromURL(szFilePath, pReaderAttrs, m_pReader.GetAddressOf());
    if (pReaderAttrs) pReaderAttrs->Release();

    if (FAILED(hr)) {
        DLOG_ERROR("VideoCapture: Failed to open video file (hr=0x%08X)", hr);
        return false;
    }

    // Select only the video stream (deselect audio to avoid buffering issues)
    m_pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    m_pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    // Configure output format: RGB32 (BGRA)
    ComPtr<IMFMediaType> pType;
    hr = MFCreateMediaType(pType.GetAddressOf());
    if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = pType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = m_pReader->SetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType.Get());

    if (FAILED(hr)) {
        DLOG_ERROR("VideoCapture: Failed to set video output to RGB32 (hr=0x%08X)", hr);
        m_pReader.Reset();
        return false;
    }

    // Read back actual format to get width/height/stride
    ComPtr<IMFMediaType> pActualType;
    hr = m_pReader->GetCurrentMediaType(
        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, pActualType.GetAddressOf());
    if (SUCCEEDED(hr)) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(pActualType.Get(), MF_MT_FRAME_SIZE, &w, &h);
        m_nWidth = w;
        m_nHeight = h;

        // Get actual stride (negative = bottom-up, typical for RGB32)
        LONG stride = 0;
        if (FAILED(pActualType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride)) || stride == 0)
            stride = -(LONG)(w * 4); // RGB32 default: bottom-up
        m_nStride = stride;
    }

    m_type = VideoFile;
    m_bStopCapture = false;
    m_bConnected = true;
    m_bNewFrame = false;

    // Start capture thread
    m_hCaptureThread = (HANDLE)_beginthreadex(nullptr, 0, CaptureThreadProc, this, 0, nullptr);
    if (!m_hCaptureThread) {
        DebugLogA("VideoCapture: Failed to create capture thread", LOG_ERROR);
        Close();
        return false;
    }

    DLOG_INFO("VideoCapture: Video file opened %ux%u", m_nWidth, m_nHeight);
    return true;
}

// ---------------------------------------------------------------------------
// Close — stop capture thread and release all resources
// ---------------------------------------------------------------------------
void VideoCaptureSource::Close()
{
    m_bStopCapture = true;
    m_bConnected = false;

    if (m_hCaptureThread) {
        WaitForSingleObject(m_hCaptureThread, 5000);
        CloseHandle(m_hCaptureThread);
        m_hCaptureThread = nullptr;
    }

    // Source reader must be released on the thread that created it (or after thread exits)
    m_pReader.Reset();

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_frameBuffer.clear();
        m_nFrameWidth = 0;
        m_nFrameHeight = 0;
    }

    m_pUploadBuffer.Reset();
    m_pGpuTexture.Reset();
    m_dx12Tex.Reset();
    m_nGpuTexWidth = 0;
    m_nGpuTexHeight = 0;
    m_nWidth = 0;
    m_nHeight = 0;
    m_nStride = 0;
    m_bNewFrame = false;
    m_type = None;

    if (m_bMFStarted) {
        MFShutdown();
        m_bMFStarted = false;
    }

    DebugLogA("VideoCapture: Closed");
}

// ---------------------------------------------------------------------------
// CaptureThreadProc — thread entry point
// ---------------------------------------------------------------------------
unsigned __stdcall VideoCaptureSource::CaptureThreadProc(void* pArg)
{
    auto* self = static_cast<VideoCaptureSource*>(pArg);

    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool bComOwner = (hrCom == S_OK);

    if (self->m_type == Webcam)
        self->CaptureLoop();
    else if (self->m_type == VideoFile)
        self->VideoFileLoop();

    if (bComOwner) CoUninitialize();
    return 0;
}

// ---------------------------------------------------------------------------
// CaptureLoop — webcam: read frames as fast as the device delivers them
// ---------------------------------------------------------------------------
void VideoCaptureSource::CaptureLoop()
{
    while (!m_bStopCapture.load()) {
        DWORD dwStreamIndex = 0, dwFlags = 0;
        LONGLONG llTimestamp = 0;
        ComPtr<IMFSample> pSample;

        HRESULT hr = m_pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, &dwStreamIndex, &dwFlags, &llTimestamp, pSample.GetAddressOf());

        if (FAILED(hr) || (dwFlags & MF_SOURCE_READERF_ERROR)) {
            DebugLogA("VideoCapture: Webcam read error, stopping", LOG_ERROR);
            m_bConnected = false;
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            m_bConnected = false;
            break;
        }

        if (!pSample) continue;
        ExtractFrameData(pSample.Get());
    }
}

// ---------------------------------------------------------------------------
// VideoFileLoop — video file: read frames paced by timestamp
// ---------------------------------------------------------------------------
void VideoCaptureSource::VideoFileLoop()
{
    LARGE_INTEGER qpcFreq, qpcStart;
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);

    while (!m_bStopCapture.load()) {
        DWORD dwStreamIndex = 0, dwFlags = 0;
        LONGLONG llTimestamp = 0; // in 100-nanosecond units
        ComPtr<IMFSample> pSample;

        HRESULT hr = m_pReader->ReadSample(
            (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
            0, &dwStreamIndex, &dwFlags, &llTimestamp, pSample.GetAddressOf());

        if (FAILED(hr) || (dwFlags & MF_SOURCE_READERF_ERROR)) {
            DebugLogA("VideoCapture: Video file read error", LOG_ERROR);
            m_bConnected = false;
            break;
        }

        if (dwFlags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (m_bLoop && !m_bStopCapture.load()) {
                // Seek back to beginning
                PROPVARIANT var;
                PropVariantInit(&var);
                var.vt = VT_I8;
                var.hVal.QuadPart = 0;
                m_pReader->SetCurrentPosition(GUID_NULL, var);
                PropVariantClear(&var);

                // Reset playback clock
                QueryPerformanceCounter(&qpcStart);
                continue;
            }
            m_bConnected = false;
            break;
        }

        if (!pSample) continue;

        // Pace playback: wait until the frame's presentation time
        if (llTimestamp > 0) {
            LARGE_INTEGER qpcNow;
            QueryPerformanceCounter(&qpcNow);
            double elapsedSec = (double)(qpcNow.QuadPart - qpcStart.QuadPart) / (double)qpcFreq.QuadPart;
            double frameSec = (double)llTimestamp / 10000000.0; // 100ns → seconds
            double waitSec = frameSec - elapsedSec;
            if (waitSec > 0.001) {
                DWORD waitMs = (DWORD)(waitSec * 1000.0);
                if (waitMs > 500) waitMs = 500; // cap to avoid long blocks during shutdown
                Sleep(waitMs);
            }
        }

        if (m_bStopCapture.load()) break;
        ExtractFrameData(pSample.Get());
    }
}

// ---------------------------------------------------------------------------
// ExtractFrameData — copy MF sample into m_frameBuffer (top-down, w*4 stride)
// ---------------------------------------------------------------------------
bool VideoCaptureSource::ExtractFrameData(IMFSample* pSample)
{
    if (!pSample) return false;

    ComPtr<IMFMediaBuffer> pBuffer;
    HRESULT hr = pSample->ConvertToContiguousBuffer(pBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    UINT w = m_nWidth;
    UINT h = m_nHeight;
    if (w == 0 || h == 0) return false;
    UINT rowBytes = w * 4;
    UINT totalBytes = rowBytes * h;

    // Try IMF2DBuffer for proper stride handling (preferred path)
    ComPtr<IMF2DBuffer> p2D;
    if (SUCCEEDED(pBuffer.As(&p2D))) {
        BYTE* pScanline0 = nullptr;
        LONG lPitch = 0;
        hr = p2D->Lock2D(&pScanline0, &lPitch);
        if (SUCCEEDED(hr)) {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_frameBuffer.size() != totalBytes)
                m_frameBuffer.resize(totalBytes);
            // Lock2D returns pScanline0 at top image row; pitch handles direction
            for (UINT y = 0; y < h; y++) {
                const BYTE* srcRow = pScanline0 + (LONG)y * lPitch;
                memcpy(m_frameBuffer.data() + y * rowBytes, srcRow, rowBytes);
            }
            m_nFrameWidth = w;
            m_nFrameHeight = h;
            p2D->Unlock2D();
            m_bNewFrame = true;
            return true;
        }
    }

    // Fallback: 1D buffer with stride from media type
    BYTE* pPixels = nullptr;
    DWORD cbMaxLen = 0, cbCurrentLen = 0;
    hr = pBuffer->Lock(&pPixels, &cbMaxLen, &cbCurrentLen);
    if (FAILED(hr)) return false;

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_frameBuffer.size() != totalBytes)
            m_frameBuffer.resize(totalBytes);

        LONG stride = m_nStride;
        UINT absStride = (UINT)(stride < 0 ? -stride : stride);
        if (absStride < rowBytes) absStride = rowBytes; // sanity
        bool bottomUp = (stride <= 0); // RGB32 default: bottom-up

        for (UINT y = 0; y < h; y++) {
            const BYTE* srcRow = bottomUp
                ? pPixels + (h - 1 - y) * absStride
                : pPixels + y * absStride;
            memcpy(m_frameBuffer.data() + y * rowBytes, srcRow, rowBytes);
        }
        m_nFrameWidth = w;
        m_nFrameHeight = h;
    }
    m_bNewFrame = true;

    pBuffer->Unlock();
    return true;
}

// ---------------------------------------------------------------------------
// UpdateTexture — upload latest frame to GPU (called on render thread)
// ---------------------------------------------------------------------------
bool VideoCaptureSource::UpdateTexture(DXContext* dx)
{
    if (!m_bNewFrame.load() || !dx)
        return false;

    // Grab frame data under lock
    UINT w, h;
    std::vector<BYTE> pixels;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_frameBuffer.empty()) return false;
        w = m_nFrameWidth;
        h = m_nFrameHeight;
        pixels = m_frameBuffer; // copy
    }
    m_bNewFrame = false;

    if (w == 0 || h == 0) return false;

    // Ensure GPU texture exists at correct size
    if (!EnsureGpuTexture(dx, w, h))
        return false;

    // Upload pixels
    return UploadFrameToGpu(dx, pixels.data(), w, h);
}

// ---------------------------------------------------------------------------
// EnsureGpuTexture — create/recreate GPU texture and SRV if size changed
// ---------------------------------------------------------------------------
bool VideoCaptureSource::EnsureGpuTexture(DXContext* dx, UINT w, UINT h)
{
    if (m_pGpuTexture && m_nGpuTexWidth == w && m_nGpuTexHeight == h)
        return true; // already correct size

    auto* device = dx->m_device.Get();
    if (!device) return false;

    // Release old resources (preserve descriptor indices if allocated)
    m_pGpuTexture.Reset();
    m_pUploadBuffer.Reset();
    m_dx12Tex.ResetResource();

    // Create GPU texture (DEFAULT heap, COPY_DEST initially)
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(m_pGpuTexture.GetAddressOf()));
    if (FAILED(hr)) {
        DebugLogA("VideoCapture: Failed to create GPU texture", LOG_ERROR);
        return false;
    }

    // Create upload buffer (row pitch aligned to 256 bytes)
    UINT rowPitch = (w * 4 + 255) & ~255u; // 256-byte aligned
    UINT uploadSize = rowPitch * h;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(m_pUploadBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        DebugLogA("VideoCapture: Failed to create upload buffer", LOG_ERROR);
        m_pGpuTexture.Reset();
        return false;
    }

    // Set up DX12Texture wrapper
    m_dx12Tex.resource = m_pGpuTexture;
    m_dx12Tex.width = w;
    m_dx12Tex.height = h;
    m_dx12Tex.depth = 1;
    m_dx12Tex.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_dx12Tex.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

    // Allocate SRV + binding block (first time only)
    if (m_dx12Tex.srvIndex == UINT_MAX) {
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = dx->AllocateSrvCpu();
        dx->AllocateSrvGpu();
        m_dx12Tex.srvIndex = dx->m_nextFreeSrvSlot - 1;

        CreateSRV2D(device, m_pGpuTexture.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, srvCpu);
        dx->CreateBindingBlockForTexture(m_dx12Tex);
    } else {
        // Recreate SRV in-place
        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = dx->GetSrvCpuHandleAt(m_dx12Tex.srvIndex);
        CreateSRV2D(device, m_pGpuTexture.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, srvCpu);
        dx->UpdateBindingBlockTexture(m_dx12Tex.bindingBlockStart, m_dx12Tex.srvIndex);
    }

    m_nGpuTexWidth = w;
    m_nGpuTexHeight = h;

    DLOG_INFO("VideoCapture: GPU texture created %ux%u", w, h);
    return true;
}

// ---------------------------------------------------------------------------
// UploadFrameToGpu — copy BGRA pixels to GPU texture via upload buffer
// ---------------------------------------------------------------------------
bool VideoCaptureSource::UploadFrameToGpu(DXContext* dx, const BYTE* pPixels, UINT w, UINT h)
{
    if (!m_pUploadBuffer || !m_pGpuTexture || !dx->m_commandList.Get())
        return false;

    UINT srcRowPitch = w * 4;
    UINT dstRowPitch = (w * 4 + 255) & ~255u; // 256-byte aligned

    // Map upload buffer and copy row-by-row (handling pitch alignment)
    BYTE* pMapped = nullptr;
    D3D12_RANGE readRange = {0, 0}; // we won't read
    HRESULT hr = m_pUploadBuffer->Map(0, &readRange, (void**)&pMapped);
    if (FAILED(hr)) return false;

    // Frame buffer is pre-normalized to top-down by ExtractFrameData
    for (UINT y = 0; y < h; y++) {
        const BYTE* srcRow = pPixels + y * srcRowPitch;
        BYTE* dstRow = pMapped + y * dstRowPitch;
        memcpy(dstRow, srcRow, srcRowPitch);
    }
    m_pUploadBuffer->Unmap(0, nullptr);

    // Transition GPU texture to COPY_DEST if needed
    dx->TransitionResource(m_dx12Tex, D3D12_RESOURCE_STATE_COPY_DEST);

    // Issue copy command
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = m_pGpuTexture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_pUploadBuffer.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = dstRowPitch;

    dx->m_commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // Transition to shader resource for rendering
    dx->TransitionResource(m_dx12Tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    return true;
}

} // namespace mdrop
