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
#include "json_utils.h"
#include "config_store.h"

extern ID3DBlob* g_pBlurVSBlob;

namespace mdrop {

// ---------------------------------------------------------------------------
// Video FX enhanced pixel shader — transforms, color effects, luma key, blending
// ---------------------------------------------------------------------------
static const char szVideoFXPS[] =
    "Texture2D<float4> tex : register(t0);\n"
    "Texture2D<float4> dest : register(t1);\n"
    "SamplerState samp : register(s0);\n"
    "cbuffer cb : register(b0) {\n"
    "    float lumaThreshold;\n"
    "    float lumaSoftness;\n"
    "    float opacity;\n"
    "    float lumaActive;\n"
    "    float2 translate;\n"      // posX, posY
    "    float scale;\n"
    "    float rotation;\n"        // radians
    "    float3 tint;\n"
    "    float brightness;\n"
    "    float contrast;\n"
    "    float saturation;\n"
    "    float hueShift;\n"        // radians
    "    float invertFlag;\n"
    "    float pixelation;\n"
    "    float mirrorH;\n"
    "    float mirrorV;\n"
    "    float chromatic;\n"
    "    float edgeDetect;\n"
    "    float blendMode;\n"
    "    float2 texelSize;\n"      // 1/width, 1/height of video texture
    "};\n"
    "\n"
    "float3 rgb2hsv(float3 c) {\n"
    "    float4 K = float4(0.0, -1.0/3.0, 2.0/3.0, -1.0);\n"
    "    float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));\n"
    "    float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));\n"
    "    float d = q.x - min(q.w, q.y);\n"
    "    float e = 1.0e-10;\n"
    "    return float3(abs(q.z + (q.w - q.y) / (6.0*d + e)), d / (q.x + e), q.x);\n"
    "}\n"
    "\n"
    "float3 hsv2rgb(float3 c) {\n"
    "    float4 K = float4(1.0, 2.0/3.0, 1.0/3.0, 3.0);\n"
    "    float3 p = abs(frac(c.xxx + K.xyz) * 6.0 - K.www);\n"
    "    return c.z * lerp(K.xxx, saturate(p - K.xxx), c.y);\n"
    "}\n"
    "\n"
    "float4 main(float2 uv : TEXCOORD0) : SV_Target {\n"
    "    // --- UV transforms: center → rotate → scale → translate → mirror → pixelate ---\n"
    "    float2 tc = uv - 0.5;\n"
    "    float cosR = cos(rotation);\n"
    "    float sinR = sin(rotation);\n"
    "    tc = float2(tc.x*cosR - tc.y*sinR, tc.x*sinR + tc.y*cosR);\n"
    "    tc /= max(scale, 0.001);\n"
    "    tc -= translate;\n"
    "    tc += 0.5;\n"
    "    if (mirrorH > 0.5) tc.x = 1.0 - tc.x;\n"
    "    if (mirrorV > 0.5) tc.y = 1.0 - tc.y;\n"
    "    if (pixelation > 0.001) {\n"
    "        float cells = lerp(512.0, 4.0, pixelation);\n"
    "        tc = floor(tc * cells) / cells;\n"
    "    }\n"
    "    // Discard if UV is out of [0,1]²\n"
    "    if (tc.x < 0 || tc.x > 1 || tc.y < 0 || tc.y > 1) return float4(0,0,0,0);\n"
    "\n"
    "    // --- Sampling (with optional chromatic aberration) ---\n"
    "    float4 col;\n"
    "    if (chromatic > 0.0001) {\n"
    "        float2 dir = tc - 0.5;\n"
    "        col.r = tex.Sample(samp, tc + dir * chromatic).r;\n"
    "        col.g = tex.Sample(samp, tc).g;\n"
    "        col.b = tex.Sample(samp, tc - dir * chromatic).b;\n"
    "        col.a = tex.Sample(samp, tc).a;\n"
    "    } else {\n"
    "        col = tex.Sample(samp, tc);\n"
    "    }\n"
    "\n"
    "    // --- Luma key ---\n"
    "    col.a = opacity;\n"
    "    if (lumaActive > 0.5) {\n"
    "        float luma = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
    "        col.a *= saturate((luma - lumaThreshold) / max(0.0001, lumaSoftness));\n"
    "    }\n"
    "\n"
    "    // --- Color: tint → brightness → contrast → saturation → hue → invert ---\n"
    "    col.rgb *= tint;\n"
    "    col.rgb += brightness;\n"
    "    col.rgb = lerp(0.5, col.rgb, contrast);\n"
    "    float grey = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
    "    col.rgb = lerp(grey, col.rgb, saturation);\n"
    "    if (abs(hueShift) > 0.001) {\n"
    "        float3 hsv = rgb2hsv(saturate(col.rgb));\n"
    "        hsv.x = frac(hsv.x + hueShift / 6.283185);\n"
    "        col.rgb = hsv2rgb(hsv);\n"
    "    }\n"
    "    if (invertFlag > 0.5) col.rgb = 1.0 - col.rgb;\n"
    "\n"
    "    // --- Edge detection (3×3 Sobel on luminance) ---\n"
    "    if (edgeDetect > 0.5) {\n"
    "        float tl = dot(tex.Sample(samp, tc + float2(-texelSize.x, -texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float t  = dot(tex.Sample(samp, tc + float2(0, -texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float tr = dot(tex.Sample(samp, tc + float2( texelSize.x, -texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float ml = dot(tex.Sample(samp, tc + float2(-texelSize.x, 0)).rgb, float3(0.299,0.587,0.114));\n"
    "        float mr = dot(tex.Sample(samp, tc + float2( texelSize.x, 0)).rgb, float3(0.299,0.587,0.114));\n"
    "        float bl = dot(tex.Sample(samp, tc + float2(-texelSize.x,  texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float b  = dot(tex.Sample(samp, tc + float2(0,  texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float br = dot(tex.Sample(samp, tc + float2( texelSize.x,  texelSize.y)).rgb, float3(0.299,0.587,0.114));\n"
    "        float gx = -tl - 2*ml - bl + tr + 2*mr + br;\n"
    "        float gy = -tl - 2*t  - tr + bl + 2*b  + br;\n"
    "        float edge = sqrt(gx*gx + gy*gy);\n"
    "        col.rgb = edge;\n"
    "    }\n"
    "\n"
    "    // --- Shader-based blend modes (modes 2-5 read destination) ---\n"
    "    int bm = (int)blendMode;\n"
    "    if (bm >= 2) {\n"
    "        float3 dst = dest.Sample(samp, uv).rgb;\n"
    "        float3 result = col.rgb;\n"
    "        if (bm == 2) result = col.rgb * dst;\n"                      // Multiply
    "        else if (bm == 3) result = 1.0 - (1.0 - col.rgb) * (1.0 - dst);\n"  // Screen
    "        else if (bm == 4) {\n"                                       // Overlay
    "            result = lerp(2.0 * col.rgb * dst, 1.0 - 2.0*(1.0-col.rgb)*(1.0-dst), step(0.5, dst));\n"
    "        }\n"
    "        else if (bm == 5) result = abs(col.rgb - dst);\n"            // Difference
    "        col.rgb = lerp(dst, result, col.a);\n"
    "        col.a = 1.0;\n"
    "    }\n"
    "\n"
    "    return col;\n"
    "}\n";

// ---------------------------------------------------------------------------
// CompileVideoFXPSOs — create 3 PSO variants for video effects
// ---------------------------------------------------------------------------
void Engine::CompileVideoFXPSOs()
{
    if (!m_lpDX || !m_lpDX->m_rootSignature.Get() || !g_pBlurVSBlob)
        return;

    ID3DBlob* psBlob = nullptr;
    ID3DBlob* pErrors = nullptr;
    HRESULT hr = D3DCompile(szVideoFXPS, strlen(szVideoFXPS), nullptr, nullptr, nullptr,
                            "main", "ps_5_0", D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY, 0,
                            &psBlob, &pErrors);
    if (FAILED(hr)) {
        if (pErrors) { DebugLogA((const char*)pErrors->GetBufferPointer(), LOG_ERROR); pErrors->Release(); }
        DebugLogA("DX12: VideoFX PSO: PS compile FAILED", LOG_ERROR);
        return;
    }
    if (pErrors) pErrors->Release();

    auto* device = m_lpDX->m_device.Get();
    auto* rootSig = m_lpDX->m_rootSignature.Get();

    // Build base PSO desc
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSig;
    desc.VS = { g_pBlurVSBlob->GetBufferPointer(), g_pBlurVSBlob->GetBufferSize() };
    desc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
    desc.InputLayout = { g_MyVertexLayout, _countof(g_MyVertexLayout) };
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = 1;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

    // PSO 0: Alpha blend (SRC_ALPHA / INV_SRC_ALPHA)
    desc.BlendState.RenderTarget[0].BlendEnable           = TRUE;
    desc.BlendState.RenderTarget[0].SrcBlend              = D3D12_BLEND_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].DestBlend             = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOp               = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha         = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha        = D3D12_BLEND_INV_SRC_ALPHA;
    desc.BlendState.RenderTarget[0].BlendOpAlpha          = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pVideoFX_PSO_Alpha));
    if (FAILED(hr)) DebugLogA("DX12: VideoFX PSO Alpha FAILED", LOG_ERROR);

    // PSO 1: Additive blend (SRC_ALPHA / ONE)
    desc.BlendState.RenderTarget[0].DestBlend     = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pVideoFX_PSO_Additive));
    if (FAILED(hr)) DebugLogA("DX12: VideoFX PSO Additive FAILED", LOG_ERROR);

    // PSO 2: Solid / no-blend (for shader-based blend modes 2-5)
    desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&m_pVideoFX_PSO_Solid));
    if (FAILED(hr)) DebugLogA("DX12: VideoFX PSO Solid FAILED", LOG_ERROR);

    psBlob->Release();
    DebugLogA("DX12: VideoFX PSOs: created");
}

// ---------------------------------------------------------------------------
// ResolveAudio — apply audio-reactive modulation to a base value
// ---------------------------------------------------------------------------
static float ResolveAudio(float base, const AudioLink& link, const td_mysounddata& snd)
{
    if (link.source == 0) return base;
    float audio;
    if (link.source <= 3)
        audio = snd.imm_rel[link.source - 1];
    else
        audio = (snd.imm_rel[0] + snd.imm_rel[1] + snd.imm_rel[2]) / 3.0f;
    return base + (audio - 1.0f) * link.intensity;
}

// ---------------------------------------------------------------------------
// CompositeVideoInputFX — draw video with transforms, effects, audio, blending
// ---------------------------------------------------------------------------
void Engine::CompositeVideoInputFX(bool isBackground, DX12Texture& tex, UINT srcW, UINT srcH)
{
    if (!tex.IsValid() || !srcW || !srcH)
        return;

    // Fast path: if all effects at defaults and blend=alpha, use original simple path
    if (m_videoFX.IsDefault() && m_pSpoutInputPSO) {
        CompositeVideoInput(isBackground, tex, srcW, srcH);
        return;
    }

    // Need at least the alpha PSO
    if (!m_pVideoFX_PSO_Alpha)
        return;

    auto* cmdList = m_lpDX->GetActiveCmdList();
    if (!cmdList)
        return;

    // Resolve audio-reactive values
    float posX     = ResolveAudio(m_videoFX.posX,       m_videoFX.arPosX,       mysound);
    float posY     = ResolveAudio(m_videoFX.posY,       m_videoFX.arPosY,       mysound);
    float fxScale  = ResolveAudio(m_videoFX.scale,      m_videoFX.arScale,      mysound);
    float rot      = ResolveAudio(m_videoFX.rotation,   m_videoFX.arRotation,   mysound);
    float bright   = ResolveAudio(m_videoFX.brightness, m_videoFX.arBrightness, mysound);
    float sat      = ResolveAudio(m_videoFX.saturation, m_videoFX.arSaturation, mysound);
    float chrom    = ResolveAudio(m_videoFX.chromatic,  m_videoFX.arChromatic,  mysound);

    // Clamp resolved values
    fxScale = max(0.01f, fxScale);
    chrom = max(0.0f, chrom);

    // Select PSO by blend mode
    int bm = m_videoFX.blendMode;
    ID3D12PipelineState* pso;
    if (bm == 1)
        pso = m_pVideoFX_PSO_Additive.Get();
    else if (bm >= 2)
        pso = m_pVideoFX_PSO_Solid.Get();
    else
        pso = m_pVideoFX_PSO_Alpha.Get();
    if (!pso) return;

    // For shader-based blends (2-5), copy current RT to dest texture
    // (TODO: allocate m_dx12VideoFXDest and copy here for modes 2-5)

    // Transition input texture to shader resource
    m_lpDX->TransitionResource(tex, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    // Set PSO + root signature + descriptor heaps
    cmdList->SetPipelineState(pso);
    cmdList->SetGraphicsRootSignature(m_lpDX->m_rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { m_lpDX->m_srvHeap.Get() };
    cmdList->SetDescriptorHeaps(1, heaps);

    // Upload constant buffer matching the enhanced shader layout
    float rotRad = rot * 3.14159265f / 180.0f;
    float hueRad = m_videoFX.hueShift * 3.14159265f / 180.0f;
    struct {
        float lumaThreshold, lumaSoftness, opacity, lumaActive;
        float translateX, translateY, scale, rotation;
        float tintR, tintG, tintB, brightness;
        float contrast, saturation, hueShift, invertFlag;
        float pixelation, mirrorH, mirrorV, chromatic;
        float edgeDetect, blendMode, texelSizeX, texelSizeY;
    } cbData = {
        m_fSpoutInputLumaThreshold,
        m_fSpoutInputLumaSoftness,
        m_fSpoutInputOpacity,
        m_bSpoutInputLumaKey ? 1.0f : 0.0f,
        posX, posY, fxScale, rotRad,
        m_videoFX.tintR, m_videoFX.tintG, m_videoFX.tintB, bright,
        m_videoFX.contrast, sat, hueRad, m_videoFX.invert ? 1.0f : 0.0f,
        m_videoFX.pixelation, m_videoFX.mirrorH ? 1.0f : 0.0f, m_videoFX.mirrorV ? 1.0f : 0.0f, chrom,
        m_videoFX.edgeDetect ? 1.0f : 0.0f, (float)bm, 1.0f / (float)srcW, 1.0f / (float)srcH
    };
    D3D12_GPU_VIRTUAL_ADDRESS cbAddr = m_lpDX->UploadConstantBuffer(&cbData, sizeof(cbData));
    if (cbAddr)
        cmdList->SetGraphicsRootConstantBufferView(0, cbAddr);

    // Bind input texture via binding block
    cmdList->SetGraphicsRootDescriptorTable(1, m_lpDX->GetBindingBlockGpuHandle(tex));

    // Compute aspect-ratio-preserving quad (cover mode)
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
        float s = inputAspect / targetAspect;
        left = -s; right = s;
    } else {
        float s = targetAspect / inputAspect;
        top = s; bottom = -s;
    }

    MYVERTEX v[4];
    ZeroMemory(v, sizeof(v));
    v[0].x = left;  v[0].y = top;    v[0].z = 0.f; v[0].Diffuse = 0xFFFFFFFF; v[0].tu = 0.f; v[0].tv = 0.f;
    v[1].x = right; v[1].y = top;    v[1].z = 0.f; v[1].Diffuse = 0xFFFFFFFF; v[1].tu = 1.f; v[1].tv = 0.f;
    v[2].x = left;  v[2].y = bottom; v[2].z = 0.f; v[2].Diffuse = 0xFFFFFFFF; v[2].tu = 0.f; v[2].tv = 1.f;
    v[3].x = right; v[3].y = bottom; v[3].z = 0.f; v[3].Diffuse = 0xFFFFFFFF; v[3].tu = 1.f; v[3].tv = 1.f;
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
}

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
    DebugLogA(m_pSpoutInputPSO ? "DX12: Spout Input PSO: created" : "DX12: Spout Input PSO: create FAILED", m_pSpoutInputPSO ? LOG_INFO : LOG_ERROR);
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

    auto* cmdList = m_lpDX->GetActiveCmdList();
    if (!cmdList)
        return;

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
    m_lpDX->DrawVertices(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, v, 4, sizeof(MYVERTEX), cmdList);
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
    wchar_t buf[256];

    m_bSpoutInputEnabled = Config().GetInt(L"SpoutInput", L"Enabled", 0) != 0;
    m_bSpoutInputOnTop = Config().GetInt(L"SpoutInput", L"OnTop", 0) != 0;

    Config().GetStringTo(L"SpoutInput", L"Opacity", L"1.0", buf, 64);
    m_fSpoutInputOpacity = (float)_wtof(buf);
    if (m_fSpoutInputOpacity < 0.f) m_fSpoutInputOpacity = 0.f;
    if (m_fSpoutInputOpacity > 1.f) m_fSpoutInputOpacity = 1.f;

    m_bSpoutInputLumaKey = Config().GetInt(L"SpoutInput", L"LumaKey", 0) != 0;

    Config().GetStringTo(L"SpoutInput", L"LumaThreshold", L"0.1", buf, 64);
    m_fSpoutInputLumaThreshold = (float)_wtof(buf);
    if (m_fSpoutInputLumaThreshold < 0.f) m_fSpoutInputLumaThreshold = 0.f;
    if (m_fSpoutInputLumaThreshold > 1.f) m_fSpoutInputLumaThreshold = 1.f;

    Config().GetStringTo(L"SpoutInput", L"LumaSoftness", L"0.1", buf, 64);
    m_fSpoutInputLumaSoftness = (float)_wtof(buf);
    if (m_fSpoutInputLumaSoftness < 0.f) m_fSpoutInputLumaSoftness = 0.f;
    if (m_fSpoutInputLumaSoftness > 1.f) m_fSpoutInputLumaSoftness = 1.f;

    Config().GetStringTo(L"SpoutInput", L"SenderName", L"", m_szSpoutInputSender, 256);

    // Video input source (new unified setting; backward compat: Enabled=1 → Source=Spout)
    int source = Config().GetInt(L"SpoutInput", L"Source", -1);
    if (source >= 0) {
        m_nVideoInputSource = source;
    } else {
        // Legacy: no Source key — derive from Enabled
        m_nVideoInputSource = m_bSpoutInputEnabled ? VID_SOURCE_SPOUT : VID_SOURCE_NONE;
    }
    m_bSpoutInputEnabled = (m_nVideoInputSource != VID_SOURCE_NONE);

    Config().GetStringTo(L"SpoutInput", L"WebcamDevice", L"", m_szWebcamDevice, 256);
    Config().GetStringTo(L"SpoutInput", L"VideoFile", L"", m_szVideoFile, MAX_PATH);
    m_bVideoLoop = Config().GetInt(L"SpoutInput", L"VideoLoop", 1) != 0;

    // ── VideoFX settings ──
    //
    // MovedToJson is the explicit record that the parameters have left the
    // INI. Once it is set the values below are stale by definition and are
    // read only to seed a first profile on an install coming from a build
    // that kept them here.
    const bool bMovedToJson = Config().GetInt(L"VideoFX", L"MovedToJson", 0) != 0;

    auto readF = [&](const wchar_t* key, float def) -> float {
        Config().GetStringTo(L"VideoFX", key, L"", buf, 64);
        return buf[0] ? (float)_wtof(buf) : def;
    };
    // Read only to migrate an install coming from a build that kept these
    // here. With the flag set they are stale by definition, and reading them
    // would put old values back over the JSON.
    if (!bMovedToJson) {
    m_videoFX.posX       = readF(L"PosX", 0);
    m_videoFX.posY       = readF(L"PosY", 0);
    m_videoFX.scale      = readF(L"Scale", 1.0f);
    m_videoFX.rotation   = readF(L"Rotation", 0);
    m_videoFX.mirrorH    = Config().GetInt(L"VideoFX", L"MirrorH", 0) != 0;
    m_videoFX.mirrorV    = Config().GetInt(L"VideoFX", L"MirrorV", 0) != 0;
    m_videoFX.tintR      = readF(L"TintR", 1);
    m_videoFX.tintG      = readF(L"TintG", 1);
    m_videoFX.tintB      = readF(L"TintB", 1);
    m_videoFX.brightness = readF(L"Brightness", 0);
    m_videoFX.contrast   = readF(L"Contrast", 1.0f);
    m_videoFX.saturation = readF(L"Saturation", 1.0f);
    m_videoFX.hueShift   = readF(L"HueShift", 0);
    m_videoFX.invert     = Config().GetInt(L"VideoFX", L"Invert", 0) != 0;
    m_videoFX.pixelation = readF(L"Pixelation", 0);
    m_videoFX.chromatic  = readF(L"Chromatic", 0);
    m_videoFX.edgeDetect = Config().GetInt(L"VideoFX", L"EdgeDetect", 0) != 0;
    m_videoFX.blendMode  = Config().GetInt(L"VideoFX", L"BlendMode", 0);

    // Audio-reactive links
    auto readAR = [&](const wchar_t* prefix, AudioLink& ar) {
        wchar_t k[64];
        swprintf(k, 64, L"%s_Source", prefix);
        ar.source = Config().GetInt(L"VideoFX", k, 0);
        swprintf(k, 64, L"%s_Intensity", prefix);
        ar.intensity = readF(k, 0.5f);
    };
    readAR(L"AR_PosX",       m_videoFX.arPosX);
    readAR(L"AR_PosY",       m_videoFX.arPosY);
    readAR(L"AR_Scale",      m_videoFX.arScale);
    readAR(L"AR_Rotation",   m_videoFX.arRotation);
    readAR(L"AR_Brightness", m_videoFX.arBrightness);
    readAR(L"AR_Saturation", m_videoFX.arSaturation);
    readAR(L"AR_Chromatic",  m_videoFX.arChromatic);
    }   // !bMovedToJson

    // VFX Profile startup settings. These stay in the INI: they name WHICH
    // profile to use, so they cannot themselves live in the profile store.
    m_bEnableVFXStartup = Config().GetInt(L"VideoFX", L"bEnableVFXStartup", 0) != 0;
    Config().GetStringTo(L"VideoFX", L"szVFXStartup", L"", m_szVFXStartup, MAX_VFX_PROFILE_NAME);
    // Renamed from bEnableVFXStartupSavingOnClose, which defaulted to 1 and
    // drove an auto-save that no longer exists. The old key is deliberately not
    // read: inheriting a 1 from it would silently start overwriting profiles.
    m_bEnableVFXSaveOnExit = Config().GetInt(L"VideoFX", L"bEnableVFXSaveOnExit", 0) != 0;

    // Fold any videofx/<name>.json files from an older build into the store.
    // Safe to run every start: it only adds names the store does not have.
    m_vfxProfiles.MigrateFolder();

    // MovedToJson recorded that the parameters had left the INI. They have,
    // and there is no way back, so the flag has nothing left to gate -- the
    // [VideoFX] parameter keys above are read only to seed a first profile.
    if (!bMovedToJson)
        Config().SetString(L"VideoFX", L"MovedToJson", L"1");

    // Video effects start at their defaults. The ONLY thing that changes that
    // is "Load on startup" naming a profile.
    //
    // There used to be a live-state file rewritten on every parameter change
    // and read back at startup, which meant effects persisted across restarts
    // with no setting governing it: no profile selected, no profile even
    // existing, every box unchecked, and the values still came back. Whatever
    // an install had picked up -- an INI migration, something driving it over
    // IPC -- reappeared every launch with nobody remembering setting it.
    m_videoFX = VideoEffectParams{};
    m_szCurrentVFXProfile[0] = 0;

    if (m_bEnableVFXStartup && m_szVFXStartup[0]) {
        if (LoadVideoFXProfile(m_szVFXStartup))
            MarkVideoFXSaved();          // just loaded -> nothing to save
        else
            DLOG_WARN("VideoFX: startup profile '%S' not found in the store", m_szVFXStartup);
    }
}

void Engine::SaveSpoutInputSettings()
{
    wchar_t buf[64];

    Config().SetString(L"SpoutInput", L"Enabled", m_bSpoutInputEnabled ? L"1" : L"0");
    Config().SetString(L"SpoutInput", L"OnTop", m_bSpoutInputOnTop ? L"1" : L"0");

    swprintf(buf, 64, L"%.2f", m_fSpoutInputOpacity);
    Config().SetString(L"SpoutInput", L"Opacity", buf);

    Config().SetString(L"SpoutInput", L"LumaKey", m_bSpoutInputLumaKey ? L"1" : L"0");

    swprintf(buf, 64, L"%.2f", m_fSpoutInputLumaThreshold);
    Config().SetString(L"SpoutInput", L"LumaThreshold", buf);

    swprintf(buf, 64, L"%.2f", m_fSpoutInputLumaSoftness);
    Config().SetString(L"SpoutInput", L"LumaSoftness", buf);

    Config().SetString(L"SpoutInput", L"SenderName", m_szSpoutInputSender);

    // Video input source
    wchar_t srcBuf[8];
    swprintf(srcBuf, 8, L"%d", m_nVideoInputSource);
    Config().SetString(L"SpoutInput", L"Source", srcBuf);

    Config().SetString(L"SpoutInput", L"WebcamDevice", m_szWebcamDevice);
    Config().SetString(L"SpoutInput", L"VideoFile", m_szVideoFile);
    Config().SetString(L"SpoutInput", L"VideoLoop", m_bVideoLoop ? L"1" : L"0");

    // ── VideoFX settings ──
    //
    // The effect parameters are NOT written here -- they live in vfxprofiles.json
    // and only when a profile is saved. All that stays in the INI is which
    // profile to load at startup, and whether to write it back on exit.
    Config().SetString(L"VideoFX", L"bEnableVFXStartup", m_bEnableVFXStartup ? L"1" : L"0");
    Config().SetString(L"VideoFX", L"szVFXStartup", m_szVFXStartup);
    Config().SetString(L"VideoFX", L"bEnableVFXSaveOnExit", m_bEnableVFXSaveOnExit ? L"1" : L"0");

    // Keys from earlier builds that nothing reads any more. szCurrentVFXProfile
    // recorded which profile was loaded, which no longer survives a restart --
    // that is what "Load on startup" is for, and having both was how a profile
    // came back without anyone asking. bEnableVFXStartupSavingOnClose drove an
    // auto-save that is gone.
    Config().Remove(L"VideoFX", L"szCurrentVFXProfile");
    Config().Remove(L"VideoFX", L"bEnableVFXStartupSavingOnClose");
}

// The Video FX profile store moved to vfx_profile_store.h / .cpp. It lived
// here only because the video effects rendering code above did, and a store
// that just reads and writes a file has no business inside a 1,500 line file
// about Spout. Engine keeps the two operations that bridge it to live state
// (SaveVideoFXProfile / LoadVideoFXProfile, in engine_video_effects_ui.cpp).

} // namespace mdrop
