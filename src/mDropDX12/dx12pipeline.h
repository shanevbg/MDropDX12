#ifndef DX12PIPELINE_H
#define DX12PIPELINE_H

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// PSO identifiers
enum DX12PsoId {
    PSO_SOLID_WFVERTEX = 0,     // Solid untextured (WFVERTEX, tri-strip, no blend)
    PSO_ALPHABLEND_WFVERTEX,    // Alpha blend (WFVERTEX, tri-strip, src/invsrc alpha)
    PSO_TEXTURED_MYVERTEX,      // Textured solid (MYVERTEX layout, for warp pass)
    PSO_TEXTURED_SPRITEVERTEX,  // Textured solid (SPRITEVERTEX layout, for comp pass)
    PSO_LINE_ALPHABLEND_WFVERTEX,   // Line topology + SrcAlpha/InvSrcAlpha (waveforms)
    PSO_LINE_ADDITIVE_WFVERTEX,     // Line topology + SrcAlpha/One (additive waveforms)
    PSO_POINT_ALPHABLEND_WFVERTEX,  // Point topology + SrcAlpha/InvSrcAlpha (dot waveforms)
    PSO_POINT_ADDITIVE_WFVERTEX,    // Point topology + SrcAlpha/One (additive dot waveforms)
    PSO_ADDITIVE_WFVERTEX,          // Triangle + SrcAlpha/One (additive shapes, untextured)
    PSO_ADDITIVE_SPRITEVERTEX,      // Triangle + SrcAlpha/One (additive shapes, textured, wrap sampler)
    PSO_TEXTURED_CLAMP_SPRITEVERTEX,  // Triangle + SrcAlpha/InvSrcAlpha (textured, clamp sampler)
    PSO_ADDITIVE_CLAMP_SPRITEVERTEX,  // Triangle + SrcAlpha/One (additive, textured, clamp sampler)
    PSO_PREMULALPHA_SPRITEVERTEX,   // Triangle + One/InvSrcAlpha (premultiplied alpha, textured)
    PSO_ONEONE_SPRITEVERTEX,        // Triangle + One/One (pure additive, textured — title text)
    PSO_DARKEN_SPRITEVERTEX,        // Triangle + Zero/InvSrcColor (darken/shadow, textured — title shadow)
    PSO_ALPHABLEND_SPRITEVERTEX,    // Triangle + SrcAlpha/InvSrcAlpha (alpha blend, textured — sprites)
    PSO_SRCCOLOR_SPRITEVERTEX,      // Triangle + SrcColor/InvSrcColor (MD sprite blendmode 3)
    PSO_MULTIPLY_SPRITEVERTEX,      // Triangle + DestColor/Zero (MD3 sprite blend 5)
    PSO_SUBTRACT_SPRITEVERTEX,      // Triangle + One/One REV_SUBTRACT (MD3 sprite blend 6)
    PSO_ZERO_SPRITEVERTEX,          // Triangle + Zero/Zero (MD3 sprite blend 8, cut-off)
    PSO_INVERT_SPRITEVERTEX,        // 1-src via shader + One/Zero (MD3 sprite blend 7)
    PSO_DARKEN9_SPRITEVERTEX,       // dest*min(1,0.5+src) via shader + Zero/SrcColor (MD3 blend 9)
    PSO_DAMP_WFVERTEX,              // Triangle + SrcAlpha/InvSrcAlpha, RGB write mask only
                                    // (the feedback damp: a black quad at alpha 1-k
                                    //  leaves dst*k without touching VS1's alpha)
    PSO_COUNT
};

// Input layout descriptions for each vertex format
// WFVERTEX: float3 POSITION + DWORD COLOR (16 bytes)
static const D3D12_INPUT_ELEMENT_DESC g_WfVertexLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// SPRITEVERTEX: float3 POSITION + DWORD COLOR + float2 TEXCOORD0 (24 bytes)
static const D3D12_INPUT_ELEMENT_DESC g_SpriteVertexLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// MYVERTEX: float3 POSITION + DWORD COLOR + float2 TEX0 + float2 TEX1 + float2 TEX2 (40 bytes)
static const D3D12_INPUT_ELEMENT_DESC g_MyVertexLayout[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,     0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,       0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
};

// Passthrough shaders (embedded HLSL compiled at init time)
// VS: position passthrough + .bgra color swizzle (DX9 D3DCOLOR is BGRA in memory)
static const char g_szPassthroughVS[] =
    "struct VSInput {\n"
    "    float3 pos : POSITION;\n"
    "    float4 col : COLOR;\n"
    "};\n"
    "struct VSOutput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "};\n"
    "VSOutput main(VSInput input) {\n"
    "    VSOutput output;\n"
    "    output.pos = float4(input.pos, 1.0);\n"
    "    output.col = input.col.bgra;\n"  // swap R and B for DX9 D3DCOLOR compatibility
    "    return output;\n"
    "}\n";

// PS: output vertex color
static const char g_szPassthroughPS[] =
    "struct PSInput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    return input.col;\n"
    "}\n";

// Textured passthrough shaders (for warp + composite passes)
// VS: position passthrough + .bgra color swizzle + texcoord0 passthrough
static const char g_szTexturedVS[] =
    "struct VSInput {\n"
    "    float3 pos : POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "struct VSOutput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "VSOutput main(VSInput input) {\n"
    "    VSOutput output;\n"
    "    output.pos = float4(input.pos, 1.0);\n"
    "    output.col = input.col.bgra;\n"
    "    output.uv  = input.uv;\n"
    "    return output;\n"
    "}\n";

// PS: sample texture at texcoord0, modulate by vertex color (wrap sampler s0)
// MD3 sprite blend 7, "invert": output is 1 - src, independent of the
// destination (measured at MAE 0.0000 across the full destination range).
// Paired with a One/Zero blend so the shader result is written directly.
// Keyed texels carry alpha 0 (texmgr punches SpriteColorKey to alpha=0) and
// must be discarded, or they would write white instead of staying transparent.
static const char g_szSpriteInvertPS[] =
    "Texture2D    tex  : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "struct PSInput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    float4 c = tex.Sample(samp, input.uv) * input.col;\n"
    "    clip(c.a - 0.004);\n"
    "    return float4(1.0 - c.rgb, 1.0);\n"
    "}\n";

// MD3 sprite blend 9, "darken": output is dest * min(1, 0.5 + src), measured at
// MAE 0.0009. The shader supplies min(1, 0.5 + src) and a Zero/SrcColor blend
// multiplies it into the destination.
static const char g_szSpriteDarken9PS[] =
    "Texture2D    tex  : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "struct PSInput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    float4 c = tex.Sample(samp, input.uv) * input.col;\n"
    "    clip(c.a - 0.004);\n"
    "    return float4(saturate(0.5 + c.rgb), 1.0);\n"
    "}\n";

static const char g_szTexturedPS[] =
    "Texture2D    tex  : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "struct PSInput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    return tex.Sample(samp, input.uv) * input.col;\n"
    "}\n";

// PS: same as above but uses clamp sampler (s1) for shapes when bTexWrap=0
static const char g_szTexturedClampPS[] =
    "Texture2D    tex  : register(t0);\n"
    "SamplerState samp : register(s1);\n"
    "struct PSInput {\n"
    "    float4 pos : SV_POSITION;\n"
    "    float4 col : COLOR;\n"
    "    float2 uv  : TEXCOORD0;\n"
    "};\n"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    return tex.Sample(samp, input.uv) * input.col;\n"
    "}\n";

// Warp vertex shader: passes all MYVERTEX fields to match MilkDrop PS signatures.
// SV_POSITION is declared LAST so that the compiler assigns user semantics
// (COLOR, TEXCOORD0, TEXCOORD1) to registers 0,1,2 — matching the PS input.
// Output: float4 TEXCOORD0 = (tu, tv, tu_orig, tv_orig), float2 TEXCOORD1 = (rad, ang)
static const char g_szWarpVS[] =
    "void main(\n"
    "    float3 pos     : POSITION,\n"
    "    float4 col     : COLOR,\n"
    "    float2 uv      : TEXCOORD0,\n"
    "    float2 uv_orig : TEXCOORD1,\n"
    "    float2 rad_ang : TEXCOORD2,\n"
    "    out float4 oCol     : COLOR,\n"
    "    out float4 oUv      : TEXCOORD0,\n"
    "    out float2 oRadAng  : TEXCOORD1,\n"
    "    out float4 oPos     : SV_POSITION\n"
    ") {\n"
    "    oPos     = float4(pos, 1.0);\n"
    "    oCol     = col.bgra;\n"
    "    oUv      = float4(uv, uv_orig);\n"
    "    oRadAng  = rad_ang;\n"
    "}\n";

// Composite vertex shader: float4 UV (.xy=current, .zw=original) + float2 rad_ang
static const char g_szCompVS[] =
    "void main(\n"
    "    float3 pos     : POSITION,\n"
    "    float4 col     : COLOR,\n"
    "    float2 uv      : TEXCOORD0,\n"
    "    float2 uv_orig : TEXCOORD1,\n"
    "    float2 rad_ang : TEXCOORD2,\n"
    "    out float4 oCol     : COLOR,\n"
    "    out float4 oUv      : TEXCOORD0,\n"
    "    out float2 oRadAng  : TEXCOORD1,\n"
    "    out float4 oPos     : SV_POSITION\n"
    ") {\n"
    "    oPos     = float4(pos, 1.0);\n"
    "    oCol     = col.bgra;\n"
    "    oUv      = float4(uv, uv_orig);\n"
    "    oRadAng  = rad_ang;\n"
    "}\n";

// Blur vertex shader: simple passthrough for fullscreen quad blur passes.
// Takes MYVERTEX input (so we can reuse the same input layout), outputs only SV_POSITION + UV.
static const char g_szBlurVS[] =
    "void main(\n"
    "    float3 pos     : POSITION,\n"
    "    float4 col     : COLOR,\n"
    "    float2 uv      : TEXCOORD0,\n"
    "    float2 uv_orig : TEXCOORD1,\n"
    "    float2 rad_ang : TEXCOORD2,\n"
    "    out float2 oUv  : TEXCOORD0,\n"
    "    out float4 oPos : SV_POSITION\n"
    ") {\n"
    "    oPos = float4(pos.xy, 1, 1);\n"
    "    oUv  = uv;\n"
    "}\n";

// Pre-compiled warp/comp/blur VS bytecodes (populated by DX12CreatePipelines)
extern ID3DBlob* g_pWarpVSBlob;
extern ID3DBlob* g_pCompVSBlob;
extern ID3DBlob* g_pBlurVSBlob;

// Create all initial PSOs needed for first draw
bool DX12CreatePipelines(ID3D12Device* device, ID3D12RootSignature* rootSig,
                          DXGI_FORMAT rtvFormat,
                          ComPtr<ID3D12PipelineState> psoArray[PSO_COUNT]);

// Create a PSO from preset-compiled pixel shader bytecode.
// If pMainTexSlotOut is non-null, reflects the PS to find which t-register
// "sampler_main" (or "sampler_fc_main" etc.) maps to and writes it there.
// Returns UINT_MAX if not found.
ComPtr<ID3D12PipelineState> DX12CreatePresetPSO(
    ID3D12Device* device, ID3D12RootSignature* rootSig,
    DXGI_FORMAT rtvFormat,
    ID3DBlob* vsBlob,
    const void* psBytecode, UINT psSize,
    const D3D12_INPUT_ELEMENT_DESC* layout, UINT layoutCount,
    bool alphaBlend,
    UINT* pMainTexSlotOut = nullptr);

#endif // DX12PIPELINE_H
