// engine_spout_input.cpp — Spout video input mixing
//
// Receives video from external Spout senders and composites it
// with the visualizer output as a background or overlay layer.

#include "engine.h"
#include "utility.h"
#include "dx12pipeline.h"
#include <d3dcompiler.h>
#include <vector>
#include <string>
#include "video_capture.h"

extern ID3DBlob* g_pBlurVSBlob;

namespace mdrop {

// ---------------------------------------------------------------------------
// CompileSpoutInputPSO — luma-key + opacity pixel shader with alpha blending
// ---------------------------------------------------------------------------
void Engine::CompileSpoutInputPSO()
{
    if (!m_lpDX || !m_lpDX->m_rootSignature.Get() || !g_pBlurVSBlob)
        return;

    static const char szSpoutInputPS[] =
        "Texture2D<float4> tex : register(t0);\n"
        "SamplerState samp : register(s0);\n"
        "cbuffer cb : register(b0) {\n"
        "    float lumaThreshold;\n"
        "    float lumaSoftness;\n"
        "    float opacity;\n"
        "    float lumaActive;\n"
        "};\n"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
        "    float4 col = tex.Sample(samp, uv);\n"
        "    col.a = opacity;\n"
        "    if (lumaActive > 0.5) {\n"
        "        float luma = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
        "        col.a *= saturate((luma - lumaThreshold) / max(0.0001, lumaSoftness));\n"
        "    }\n"
        "    return col;\n"
        "}\n";

    ID3DBlob* psBlob = nullptr;
    ID3DBlob* pErrors = nullptr;
    HRESULT hr = D3DCompile(szSpoutInputPS, strlen(szSpoutInputPS), nullptr, nullptr, nullptr,
                            "main", "ps_5_0", D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0,
                            &psBlob, &pErrors);
    if (FAILED(hr)) {
        if (pErrors) { DebugLogA((const char*)pErrors->GetBufferPointer(), LOG_ERROR); pErrors->Release(); }
        DebugLogA("DX12: Spout Input PSO: PS compile FAILED", LOG_ERROR);
        return;
    }
    if (pErrors) pErrors->Release();

    m_pSpoutInputPSO = DX12CreatePresetPSO(
        m_lpDX->m_device.Get(), m_lpDX->m_rootSignature.Get(),
        DXGI_FORMAT_R8G8B8A8_UNORM, g_pBlurVSBlob,
        psBlob->GetBufferPointer(), (UINT)psBlob->GetBufferSize(),
        g_MyVertexLayout, _countof(g_MyVertexLayout), true /*alphaBlend*/);
    psBlob->Release();
    DebugLogA(m_pSpoutInputPSO ? "DX12: Spout Input PSO: created" : "DX12: Spout Input PSO: create FAILED");
}

// ---------------------------------------------------------------------------
// InitSpoutInput — create receiver, open D3D11On12 bridge
// ---------------------------------------------------------------------------
void Engine::InitSpoutInput()
{
    if (m_spoutInput) return;
    if (!m_lpDX || !m_lpDX->m_device.Get()) {
        DebugLogA("SpoutInput: No DX12 device", LOG_ERROR);
        return;
    }

    m_spoutInput = std::make_unique<SpoutInputState>();
    auto& si = *m_spoutInput;

    // Set receiver name if specified (empty = connect to first available)
    if (m_szSpoutInputSender[0] != L'\0') {
        char senderA[256] = {};
        WideCharToMultiByte(CP_ACP, 0, m_szSpoutInputSender, -1, senderA, 256, NULL, NULL);
        si.receiver.SetReceiverName(senderA);
    }

    if (!si.receiver.OpenDirectX12(
            m_lpDX->m_device.Get(),
            (IUnknown**)m_lpDX->m_commandQueue.GetAddressOf())) {
        DebugLogA("SpoutInput: OpenDirectX12 failed", LOG_ERROR);
        m_spoutInput.reset();
        return;
    }

    si.bReceiverReady = true;
    DebugLogA("SpoutInput: Receiver initialized");

    wchar_t msg[] = L"Spout Input: Enabled";
    AddNotification(msg);
}

// ---------------------------------------------------------------------------
// DestroySpoutInput — clean up receiver and textures
// ---------------------------------------------------------------------------
void Engine::DestroySpoutInput()
{
    if (!m_spoutInput) return;
    auto& si = *m_spoutInput;

    si.dx12InputTex.Reset();
    si.pReceivedTexture.Reset();

    if (si.bReceiverReady)
        si.receiver.CloseDirectX12();

    m_spoutInput.reset();
    DebugLogA("SpoutInput: Destroyed");
}

// ---------------------------------------------------------------------------
// UpdateSpoutInputTexture — per-frame: receive Spout frame, recreate on change
// ---------------------------------------------------------------------------
void Engine::UpdateSpoutInputTexture()
{
    if (!m_bSpoutInputEnabled)
        return;

    // Lazy-init: create receiver on first call (startup or source switch)
    if (!m_spoutInput || !m_spoutInput->bReceiverReady) {
        InitSpoutInput();
        if (!m_spoutInput || !m_spoutInput->bReceiverReady)
            return;
    }

    auto& si = *m_spoutInput;

    // Receive from Spout sender
    ID3D12Resource* pRaw = si.pReceivedTexture.Get();
    bool received = si.receiver.ReceiveDX12Resource(&pRaw);

    // If ReceiveDX12Resource created/replaced the resource, update our ComPtr
    if (pRaw != si.pReceivedTexture.Get()) {
        si.pReceivedTexture.Attach(pRaw); // Take ownership
    }

    // Sender changed — recreate SRV
    if (si.receiver.IsUpdated()) {
        unsigned int w = si.receiver.GetSenderWidth();
        unsigned int h = si.receiver.GetSenderHeight();

        DebugLogA("SpoutInput: Sender updated, recreating SRV");

        // Release old texture wrapper (keep descriptor indices if pre-allocated)
        si.dx12InputTex.ResetResource();

        if (si.pReceivedTexture) {
            si.dx12InputTex.resource = si.pReceivedTexture;
            si.dx12InputTex.width = w;
            si.dx12InputTex.height = h;
            si.dx12InputTex.format = DXGI_FORMAT_B8G8R8A8_UNORM;
            si.dx12InputTex.currentState = D3D12_RESOURCE_STATE_COPY_DEST;

            // Allocate SRV + binding block if not yet allocated
            if (si.dx12InputTex.srvIndex == UINT_MAX) {
                D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_lpDX->AllocateSrvCpu();
                m_lpDX->AllocateSrvGpu(); // bump GPU handle in lockstep
                si.dx12InputTex.srvIndex = m_lpDX->m_nextFreeSrvSlot - 1;

                CreateSRV2D(m_lpDX->m_device.Get(), si.dx12InputTex.resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, srvCpu);

                m_lpDX->CreateBindingBlockForTexture(si.dx12InputTex);
            } else {
                // Re-create SRV in-place at existing index
                D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = m_lpDX->GetSrvCpuHandleAt(si.dx12InputTex.srvIndex);
                CreateSRV2D(m_lpDX->m_device.Get(), si.dx12InputTex.resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, srvCpu);

                // Update binding block slot 0 to point to new SRV
                m_lpDX->UpdateBindingBlockTexture(si.dx12InputTex.bindingBlockStart, si.dx12InputTex.srvIndex);
            }

            si.nSenderWidth = w;
            si.nSenderHeight = h;

            wchar_t buf[256];
            swprintf(buf, 256, L"Spout Input: Connected %ux%u", w, h);
            AddNotification(buf);
        }
    }

    // Track connection state
    bool isConnected = received && si.pReceivedTexture;
    if (isConnected && !si.bConnected) {
        si.bConnected = true;
    } else if (!isConnected && si.bConnected) {
        si.bConnected = false;
        wchar_t msg[] = L"Spout Input: Sender disconnected";
        AddNotification(msg);
    }
}

// ---------------------------------------------------------------------------
// CompositeSpoutInput — Spout-specific wrapper: delegates to CompositeVideoInput
// ---------------------------------------------------------------------------
void Engine::CompositeSpoutInput(bool isBackground)
{
    if (!m_bSpoutInputEnabled || !m_spoutInput || !m_spoutInput->bConnected)
        return;
    if (!m_spoutInput->dx12InputTex.IsValid() || !m_pSpoutInputPSO)
        return;

    auto& si = *m_spoutInput;
    CompositeVideoInput(isBackground, si.dx12InputTex, si.nSenderWidth, si.nSenderHeight);

    // Transition back to COPY_DEST for next frame's ReceiveDX12Resource
    m_lpDX->TransitionResource(si.dx12InputTex, D3D12_RESOURCE_STATE_COPY_DEST);
}

// ---------------------------------------------------------------------------
// CompositeVideoInput — draw video quad with luma key + opacity (shared)
// ---------------------------------------------------------------------------
void Engine::CompositeVideoInput(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH)
{
    if (!tex.IsValid() || !m_pSpoutInputPSO || !srcW || !srcH)
        return;

    auto* cmdList = m_lpDX->m_commandList.Get();

    // Transition input texture to shader resource
    m_lpDX->TransitionResource(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Set PSO + root signature + descriptor heaps
    cmdList->SetPipelineState(m_pSpoutInputPSO.Get());
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Upload CBV with luma key parameters
    struct { float threshold, softness, opacity, lumaActive; } cbData = {
        m_fSpoutInputLumaThreshold,
        m_fSpoutInputLumaSoftness,
        m_fSpoutInputOpacity,
        m_bSpoutInputLumaKey ? 1.0f : 0.0f
    };
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(&cbData, sizeof(cbData));
    if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // Bind input texture via binding block
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBindingBlockGpuHandle(tex));

    // Compute aspect-ratio-preserving quad (cover mode: fill target, crop excess)
    float targetW, targetH;
    if (isBackground) {
        targetW = (float)m_nTexSizeX;
        targetH = (float)m_nTexSizeY;
    } else {
        targetW = (float)m_lpDX->m_client_width;
        targetH = (float)m_lpDX->m_client_height;
    }

    float inputAspect = (float)srcW / (float)srcH;
    float targetAspect = targetW / targetH;

    float left = -1.f, right = 1.f, top = 1.f, bottom = -1.f;
    if (inputAspect > targetAspect) {
        float scale = inputAspect / targetAspect;
        left = -scale;
        right = scale;
    } else {
        float scale = targetAspect / inputAspect;
        top = scale;
        bottom = -scale;
    }

    // Fullscreen quad using MYVERTEX (matches g_szBlurVS input layout)
    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    v[0].x = left;  v[0].y = top;    v[0].z = 0.f; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0.f; v[0].tv = 0.f;
    v[1].x = right; v[1].y = top;    v[1].z = 0.f; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1.f; v[1].tv = 0.f;
    v[2].x = left;  v[2].y = bottom; v[2].z = 0.f; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0.f; v[2].tv = 1.f;
    v[3].x = right; v[3].y = bottom; v[3].z = 0.f; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1.f; v[3].tv = 1.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX));
}

// ---------------------------------------------------------------------------
// EnumerateSpoutSenders — list available Spout senders
// ---------------------------------------------------------------------------
void Engine::EnumerateSpoutSenders(std::vector<std::string>& outNames)
{
    spoutDX12 temp;
    outNames = temp.GetSenderList();
}

// ---------------------------------------------------------------------------
// InitVideoCapture — create and open webcam or video file source
// ---------------------------------------------------------------------------
void Engine::InitVideoCapture()
{
    DestroyVideoCapture();

    m_videoCapture = std::make_unique<VideoCaptureSource>();

    bool ok = false;
    if (m_nVideoInputSource == VID_SOURCE_WEBCAM) {
        ok = m_videoCapture->OpenWebcam(m_szWebcamDevice);
        if (ok) {
            wchar_t buf[256];
            swprintf(buf, 256, L"Video Input: Webcam opened %ux%u",
                     m_videoCapture->GetWidth(), m_videoCapture->GetHeight());
            AddNotification(buf);
        } else {
            wchar_t msg[] = L"Video Input: Failed to open webcam";
            AddNotification(msg);
        }
    } else if (m_nVideoInputSource == VID_SOURCE_FILE) {
        m_videoCapture->m_bLoop = m_bVideoLoop;
        ok = m_videoCapture->OpenVideoFile(m_szVideoFile);
        if (ok) {
            wchar_t buf[256];
            swprintf(buf, 256, L"Video Input: File opened %ux%u",
                     m_videoCapture->GetWidth(), m_videoCapture->GetHeight());
            AddNotification(buf);
        } else {
            wchar_t msg[] = L"Video Input: Failed to open video file";
            AddNotification(msg);
        }
    }

    if (!ok) {
        m_videoCapture.reset();
    }
}

// ---------------------------------------------------------------------------
// DestroyVideoCapture — stop capture and release resources
// ---------------------------------------------------------------------------
void Engine::DestroyVideoCapture()
{
    if (m_videoCapture) {
        m_videoCapture->Close();
        m_videoCapture.reset();
        DebugLogA("VideoCapture: Destroyed");
    }
}

// ---------------------------------------------------------------------------
// UpdateVideoCaptureTexture — per-frame: upload latest frame to GPU
// ---------------------------------------------------------------------------
void Engine::UpdateVideoCaptureTexture()
{
    if (!m_videoCapture || !m_videoCapture->IsConnected())
        return;

    m_videoCapture->UpdateTexture(m_lpDX);
}

// ---------------------------------------------------------------------------
// INI persistence — [SpoutInput] section
// ---------------------------------------------------------------------------
void Engine::LoadSpoutInputSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[256];

    m_bSpoutInputEnabled = GetPrivateProfileIntW(L"SpoutInput", L"Enabled", 0, pIni) != 0;
    m_bSpoutInputOnTop = GetPrivateProfileIntW(L"SpoutInput", L"OnTop", 0, pIni) != 0;

    GetPrivateProfileStringW(L"SpoutInput", L"Opacity", L"1.0", buf, 64, pIni);
    m_fSpoutInputOpacity = (float)_wtof(buf);
    if (m_fSpoutInputOpacity < 0.f) m_fSpoutInputOpacity = 0.f;
    if (m_fSpoutInputOpacity > 1.f) m_fSpoutInputOpacity = 1.f;

    m_bSpoutInputLumaKey = GetPrivateProfileIntW(L"SpoutInput", L"LumaKey", 0, pIni) != 0;

    GetPrivateProfileStringW(L"SpoutInput", L"LumaThreshold", L"0.1", buf, 64, pIni);
    m_fSpoutInputLumaThreshold = (float)_wtof(buf);
    if (m_fSpoutInputLumaThreshold < 0.f) m_fSpoutInputLumaThreshold = 0.f;
    if (m_fSpoutInputLumaThreshold > 1.f) m_fSpoutInputLumaThreshold = 1.f;

    GetPrivateProfileStringW(L"SpoutInput", L"LumaSoftness", L"0.1", buf, 64, pIni);
    m_fSpoutInputLumaSoftness = (float)_wtof(buf);
    if (m_fSpoutInputLumaSoftness < 0.f) m_fSpoutInputLumaSoftness = 0.f;
    if (m_fSpoutInputLumaSoftness > 1.f) m_fSpoutInputLumaSoftness = 1.f;

    GetPrivateProfileStringW(L"SpoutInput", L"SenderName", L"", m_szSpoutInputSender, 256, pIni);

    // Video input source (new unified setting; backward compat: Enabled=1 → Source=Spout)
    int source = GetPrivateProfileIntW(L"SpoutInput", L"Source", -1, pIni);
    if (source >= 0) {
        m_nVideoInputSource = source;
    } else {
        // Legacy: no Source key — derive from Enabled
        m_nVideoInputSource = m_bSpoutInputEnabled ? VID_SOURCE_SPOUT : VID_SOURCE_NONE;
    }
    m_bSpoutInputEnabled = (m_nVideoInputSource != VID_SOURCE_NONE);

    GetPrivateProfileStringW(L"SpoutInput", L"WebcamDevice", L"", m_szWebcamDevice, 256, pIni);
    GetPrivateProfileStringW(L"SpoutInput", L"VideoFile", L"", m_szVideoFile, MAX_PATH, pIni);
    m_bVideoLoop = GetPrivateProfileIntW(L"SpoutInput", L"VideoLoop", 1, pIni) != 0;
}

void Engine::SaveSpoutInputSettings()
{
    wchar_t* pIni = GetConfigIniFile();
    wchar_t buf[64];

    WritePrivateProfileStringW(L"SpoutInput", L"Enabled", m_bSpoutInputEnabled ? L"1" : L"0", pIni);
    WritePrivateProfileStringW(L"SpoutInput", L"OnTop", m_bSpoutInputOnTop ? L"1" : L"0", pIni);

    swprintf(buf, 64, L"%.2f", m_fSpoutInputOpacity);
    WritePrivateProfileStringW(L"SpoutInput", L"Opacity", buf, pIni);

    WritePrivateProfileStringW(L"SpoutInput", L"LumaKey", m_bSpoutInputLumaKey ? L"1" : L"0", pIni);

    swprintf(buf, 64, L"%.2f", m_fSpoutInputLumaThreshold);
    WritePrivateProfileStringW(L"SpoutInput", L"LumaThreshold", buf, pIni);

    swprintf(buf, 64, L"%.2f", m_fSpoutInputLumaSoftness);
    WritePrivateProfileStringW(L"SpoutInput", L"LumaSoftness", buf, pIni);

    WritePrivateProfileStringW(L"SpoutInput", L"SenderName", m_szSpoutInputSender, pIni);

    // Video input source
    wchar_t srcBuf[8];
    swprintf(srcBuf, 8, L"%d", m_nVideoInputSource);
    WritePrivateProfileStringW(L"SpoutInput", L"Source", srcBuf, pIni);

    WritePrivateProfileStringW(L"SpoutInput", L"WebcamDevice", m_szWebcamDevice, pIni);
    WritePrivateProfileStringW(L"SpoutInput", L"VideoFile", m_szVideoFile, pIni);
    WritePrivateProfileStringW(L"SpoutInput", L"VideoLoop", m_bVideoLoop ? L"1" : L"0", pIni);
}

} // namespace mdrop
