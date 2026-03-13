#pragma once
// Embedded shader files for self-bootstrapping.
// When resources/data/*.fx files are missing, these are used as fallbacks
// and auto-extracted to disk for future runs and user editing.

struct EmbeddedShader {
    const wchar_t* filename;  // e.g. L"include.fx"
    const char* data;
};

static const char k_include_fx[] = R"(#define  M_PI   3.14159265359
#define  M_PI_2 6.28318530718
#define  M_INV_PI_2  0.159154943091895

float4 rand_frame; // random float4, updated each frame
float4 rand_preset; // random float4, updated once per *preset*
float4 _c0; // .xy: multiplier to use on UV's to paste an image fullscreen, *aspect-aware*; .zw = inverse.
float4 _c1, _c2, _c3, _c4;
float4 _c5; //.xy = scale,bias for reading blur1; .zw = scale,bias for reading blur2;
float4 _c6; //.xy = scale,bias for reading blur3; .zw = blur1_min,blur1_max
float4 _c7; // .xy ~= float2(1024,768); .zw ~= float2(1/1024.0, 1/768.0)
float4 _c8; // .xyzw ~= 0.5 + 0.5*cos(time * float4(~0.3, ~1.3, ~5, ~20))
float4 _c9; // .xyzw ~= same, but using sin()
float4 _c10; // .xyzw ~= 0.5 + 0.5*cos(time * float4(~0.005, ~0.008, ~0.013, ~0.022))
float4 _c11; // .xyzw ~= same, but using sin()
float4 _c12; // .xyz = mip info for main image (.x=#across, .y=#down, .z=avg); .w = unused
float4 _c13; //.xy = blur2_min,blur2_max; .zw = blur3_min, blur3_max.
float4 _c14 = float4(0.5, 0.5, 0, 0); // mouse

// Introduced by MDropDX12
float4 _c15 = float4(0, 0, 0, 0); // bass_smooth, mid_smooth, treb_smooth, vol_smooth
float4 _c16 = float4(1, 0, 1, 0); // vis_intensity, vis_shift, vis_version
float4 _c17 = float4(0, 0, 0, 0); // colshift_hue, colshift_saturation, colshift_brightness
float4 _c18 = float4(1, 0, 0, 0); // gamma_adj (default 1.0 = no change)
float4 _c19 = float4(0, 0, 0, 0); // iDate: year, month (0-11), day, seconds since midnight

float4 _qa; // q vars bank 1 [q1-q4]
float4 _qb; // q vars bank 2 [q5-q8]
float4 _qc; // q vars ...
float4 _qd; // q vars
float4 _qe; // q vars
float4 _qf; // q vars
float4 _qg; // q vars
float4 _qh; // q vars bank 8 [q29-q32]

// note: in general, don't use the current time w/the *dynamic* rotations!
float4x3 rot_s1; // four random, static rotations.  randomized @ preset load time.
float4x3 rot_s2; // minor translation component (<1).
float4x3 rot_s3;
float4x3 rot_s4;

float4x3 rot_d1; // four random, slowly changing rotations.
float4x3 rot_d2;
float4x3 rot_d3;
float4x3 rot_d4;
float4x3 rot_f1; // faster-changing.
float4x3 rot_f2;
float4x3 rot_f3;
float4x3 rot_f4;
float4x3 rot_vf1; // very-fast-changing.
float4x3 rot_vf2;
float4x3 rot_vf3;
float4x3 rot_vf4;
float4x3 rot_uf1; // ultra-fast-changing.
float4x3 rot_uf2;
float4x3 rot_uf3;
float4x3 rot_uf4;

float4x3 rot_rand1; // random every frame
float4x3 rot_rand2;
float4x3 rot_rand3;
float4x3 rot_rand4;

#define time     _c2.x
#define fps      _c2.y
#define frame    _c2.z
#define progress _c2.w

#define bass _c3.x
#define mid  _c3.y
#define treb _c3.z
#define vol  _c3.w

#define bass_att _c4.x
#define mid_att  _c4.y
#define treb_att _c4.z
#define vol_att  _c4.w

#define mouse _c14
#define mouse_x _c14.x
#define mouse_y _c14.y
#define mouse_pos _c14.xy
#define mouse_clicked _c14.z

#define bass_smooth _c15.x
#define mid_smooth  _c15.y
#define treb_smooth _c15.z
#define vol_smooth  _c15.w

#define vis_intensity _c16.x
#define vis_shift     _c16.y
#define vis_version   _c16.z

#define colshift_hue        _c17.x
#define colshift_saturation _c17.y
#define colshift_brightness _c17.z

#define gamma_adj _c18.x

#define idate _c19
#define idate_year _c19.x
#define idate_month _c19.y
#define idate_day _c19.z
#define idate_seconds _c19.w

#define q1 _qa.x
#define q2 _qa.y
#define q3 _qa.z
#define q4 _qa.w
#define q5 _qb.x
#define q6 _qb.y
#define q7 _qb.z
#define q8 _qb.w
#define q9 _qc.x
#define q10 _qc.y
#define q11 _qc.z
#define q12 _qc.w
#define q13 _qd.x
#define q14 _qd.y
#define q15 _qd.z
#define q16 _qd.w
#define q17 _qe.x
#define q18 _qe.y
#define q19 _qe.z
#define q20 _qe.w
#define q21 _qf.x
#define q22 _qf.y
#define q23 _qf.z
#define q24 _qf.w
#define q25 _qg.x
#define q26 _qg.y
#define q27 _qg.z
#define q28 _qg.w
#define q29 _qh.x
#define q30 _qh.y
#define q31 _qh.z
#define q32 _qh.w

#define aspect   _c0
#define texsize  _c7 // .xy = (w,h); .zw = (1/(float)w, 1/(float)h)
#define roam_cos _c8
#define roam_sin _c9
#define slow_roam_cos _c10
#define slow_roam_sin _c11
#define mip_x   _c12.x
#define mip_y   _c12.y
#define mip_xy  _c12.xy
#define mip_avg _c12.z
#define blur1_min _c6.z
#define blur1_max _c6.w
#define blur2_min _c13.x
#define blur2_max _c13.y
#define blur3_min _c13.z
#define blur3_max _c13.w
// 4 shared sampler states — only these consume s-registers (s0-s3).
// All textures use Texture2D (t-registers, limit ~128), referencing one of these.
SamplerState _samp_lw : register(s0);  // LINEAR + WRAP  (default)
SamplerState _samp_lc : register(s1);  // LINEAR + CLAMP (fc_main, blur)
SamplerState _samp_pc : register(s2);  // POINT  + CLAMP (pc_main)
SamplerState _samp_pw : register(s3);  // POINT  + WRAP  (pw_main)

// Legacy compatibility: sampler2D/sampler/sampler3D → Texture2D/Texture3D
#define sampler2D Texture2D
#define sampler   Texture2D
#define sampler3D Texture3D

// tex2D/tex3D macros — default to LINEAR+WRAP.
// Special-mode samplers (fc_main, pc_main, pw_main, blur) are text-substituted
// in LoadShaderFromMemory to bypass these macros and use the correct SamplerState.
#define tex2D(t, uv)      t.Sample(_samp_lw, uv)
#define tex2Dlod(t, v)    t.SampleLevel(_samp_lw, (v).xy, (v).w)
#define tex2Dbias(t, v)   t.SampleBias(_samp_lw, (v).xy, (v).w)
#define tex3D(t, uvw)     t.Sample(_samp_lw, uvw)
#define tex3Dlod(t, v)    t.SampleLevel(_samp_lw, (v).xyz, (v).w)
#define tex2d(t, uv)      t.Sample(_samp_lw, uv)
#define tex3d(t, uvw)     t.Sample(_samp_lw, uvw)

#define lum(x) (dot(x,float3(0.32,0.49,0.29)))

// NaN-safe normalize: DX9 SM3.0 normalize(0) returns 0; DX12 SM5.0 returns NaN.
// Preset code that calls normalize() on a zero-length vector/scalar would get NaN
// in DX12, propagating through the feedback loop and darkening the image.
float  _safe_normalize(float  x) { return sign(x); }
float2 _safe_normalize(float2 x) { float d = dot(x,x); return d > 0 ? x * rsqrt(d) : (float2)0; }
float3 _safe_normalize(float3 x) { float d = dot(x,x); return d > 0 ? x * rsqrt(d) : (float3)0; }
float4 _safe_normalize(float4 x) { float d = dot(x,x); return d > 0 ? x * rsqrt(d) : (float4)0; }

// NaN-safe sqrt: DX9 SM2.0 compiles sqrt(x) as x*rsq(x), where rsq = 1/sqrt(|x|).
// For negative x this gives sign(x)*sqrt(|x|) — a NEGATIVE result. Expressions like
// mus = .2/(sqrt(x)+.2) then produce negative mus, which UNORM clamps to 0.
// Using abs(x) would always return positive sqrt, changing the sign of downstream
// expressions and shifting feedback equilibrium (making presets too dark or bright).
// Using max(0,x) returns 0, making mus=1.0 (too bright).
// Correct emulation: sign(x)*sqrt(|x|) matches DX9's x*rsq(x) exactly.
// For x=0: sign(0)=0, so result is 0 (no NaN, no division-by-zero risk from sqrt).
float  _safe_sqrt(float  x) { return sign(x) * sqrt(abs(x)); }
float2 _safe_sqrt(float2 x) { return sign(x) * sqrt(abs(x)); }
float3 _safe_sqrt(float3 x) { return sign(x) * sqrt(abs(x)); }
float4 _safe_sqrt(float4 x) { return sign(x) * sqrt(abs(x)); }

// NaN-safe log: DX9 log(x<=0) returns a large negative; DX12 returns -inf or NaN.
// Clamp to a tiny positive value to avoid -inf/NaN propagation.
float  _safe_log(float  x) { return log(max(1e-30, x)); }
float2 _safe_log(float2 x) { return log(max(1e-30, x)); }
float3 _safe_log(float3 x) { return log(max(1e-30, x)); }
float4 _safe_log(float4 x) { return log(max(1e-30, x)); }

// NaN-safe tan: tan() returns ±inf at poles (π/2 + nπ). In DX9, inf*0 often
// returned 0 (non-IEEE). In DX12, inf*0 = NaN (IEEE 754), which propagates
// through UV calculations and creates black holes in the feedback loop.
// Clamp to large finite value so inf*0 → large*0 = 0 instead of NaN.
float  _safe_tan(float  x) { return clamp(tan(x), -1e4, 1e4); }
float2 _safe_tan(float2 x) { return clamp(tan(x), -1e4, 1e4); }
float3 _safe_tan(float3 x) { return clamp(tan(x), -1e4, 1e4); }
float4 _safe_tan(float4 x) { return clamp(tan(x), -1e4, 1e4); }

// NaN-safe pow: pow(negative, non-integer) returns NaN in DX12.
// DX9 often returned abs(x)^y or 0 for negative bases.
float  _safe_pow(float  x, float  y) { return pow(abs(x), y); }
float2 _safe_pow(float2 x, float2 y) { return pow(abs(x), y); }
float3 _safe_pow(float3 x, float3 y) { return pow(abs(x), y); }
float4 _safe_pow(float4 x, float4 y) { return pow(abs(x), y); }

// NaN-safe asin/acos: return NaN for inputs outside [-1,1] in DX12.
// DX9 hardware typically clamped internally.
float  _safe_asin(float  x) { return asin(clamp(x, -1.0, 1.0)); }
float2 _safe_asin(float2 x) { return asin(clamp(x, -1.0, 1.0)); }
float3 _safe_asin(float3 x) { return asin(clamp(x, -1.0, 1.0)); }
float4 _safe_asin(float4 x) { return asin(clamp(x, -1.0, 1.0)); }
float  _safe_acos(float  x) { return acos(clamp(x, -1.0, 1.0)); }
float2 _safe_acos(float2 x) { return acos(clamp(x, -1.0, 1.0)); }
float3 _safe_acos(float3 x) { return acos(clamp(x, -1.0, 1.0)); }
float4 _safe_acos(float4 x) { return acos(clamp(x, -1.0, 1.0)); }

#define GetMain(uv) (tex2D(sampler_main,uv).xyz)
#define GetPixel(uv) (tex2D(sampler_main,uv).xyz)
#define GetBlur1(uv) (sampler_blur1.Sample(_samp_lc,uv).xyz*_c5.x + _c5.y)
#define GetBlur2(uv) (sampler_blur2.Sample(_samp_lc,uv).xyz*_c5.z + _c5.w)
#define GetBlur3(uv) (sampler_blur3.Sample(_samp_lc,uv).xyz*_c6.x + _c6.y)

// previous-frame-image samplers (Texture2D, no s-register consumed):
Texture2D sampler_main;
Texture2D sampler_fc_main;
Texture2D sampler_pc_main;
Texture2D sampler_fw_main;
Texture2D sampler_pw_main;
#define sampler_FC_main sampler_fc_main
#define sampler_PC_main sampler_pc_main
#define sampler_FW_main sampler_fw_main
#define sampler_PW_main sampler_pw_main

// built-in noise textures:
Texture2D sampler_noise_lq;
Texture2D sampler_noise_lq_lite;
Texture2D sampler_noise_mq;
Texture2D sampler_noise_hq;
Texture3D sampler_noisevol_lq;
Texture3D sampler_noisevol_hq;
float4 texsize_noise_lq;
float4 texsize_noise_lq_lite;
float4 texsize_noise_mq;
float4 texsize_noise_hq;
float4 texsize_noisevol_lq;
float4 texsize_noisevol_hq;

// Shadertoy-compatible noise textures:
Texture2D sampler_noise_lq_st;
Texture2D sampler_noise_mq_st;
Texture2D sampler_noise_hq_st;
Texture3D sampler_noisevol_lq_st;
Texture3D sampler_noisevol_hq_st;

// user/random disk textures:
Texture2D sampler_rand00;
Texture2D sampler_rand01;
Texture2D sampler_rand02;
Texture2D sampler_rand03;

// Shadertoy feedback/buffer/audio textures:
Texture2D sampler_feedback;
Texture2D sampler_image;
Texture2D sampler_bufferB;
Texture2D sampler_bufferC;
Texture2D sampler_bufferD;
Texture2D sampler_audio;
#define sampler_fft sampler_audio

#define HAS_FFT_PEAK 1

// Get FFT magnitude at normalized position [0..1] in the spectrum
// 0.0 = lowest frequency (DC), 1.0 = highest frequency (~22kHz)
float get_fft(float pos) {
    return sqrt(tex2D(sampler_audio, float2(saturate(pos), 0.25)).x);
}

// Get FFT magnitude at a specific frequency in Hz
float get_fft_hz(float freq) {
    return get_fft(freq / 22050.0);
}

// Get peak-hold FFT magnitude at normalized position [0..1]
float get_fft_peak(float pos) {
    return sqrt(tex2D(sampler_audio, float2(saturate(pos), 0.75)).x);
}

// Get peak-hold FFT magnitude at a specific frequency in Hz
float get_fft_peak_hz(float freq) {
    return get_fft_peak(freq / 22050.0);
}

// procedural blur textures:
Texture2D sampler_blur1;
Texture2D sampler_blur2;
Texture2D sampler_blur3;

float3 shiftHSV(float3 c)
{
    float3 rgb = c;
    if (colshift_hue != 0.0 || colshift_saturation != 0.0 || colshift_brightness != 0.0)
    {
        float4 K = float4(0, -1.0 / 3.0, 2.0 / 3.0, -1);
        float4 p = lerp(float4(c.bg, K.wz), float4(c.gb, K.xy), step(c.b, c.g));
        float4 q = lerp(float4(p.xyw, c.r), float4(c.r, p.yzx), step(p.x, c.r));
        float d = q.x - min(q.w, q.y), e = 1e-10;
        float h = frac(abs(q.z + (q.w - q.y) / (6.0 * d + e)) + colshift_hue * 0.5);
        float s = d / (q.x + e);
        float v = q.x;
        s = (colshift_saturation <= 0.0) ? s * (1.0 + colshift_saturation) : s + (1.0 - s) * colshift_saturation;
        v = (colshift_brightness <= 0.0) ? v * (1.0 + colshift_brightness) : v + (1.0 - v) * colshift_brightness;
        float3 t = abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);
        rgb = v * lerp(float3(1, 1, 1), saturate(t - 1.0), s);
    }
    return rgb;
}

// Redirect NaN-producing intrinsics to safe versions for preset code.
// Defined AFTER _safe_* functions so the real intrinsics are used inside their bodies
// (preprocessor macros only affect later text in the translation unit).
#define sqrt _safe_sqrt
#define log _safe_log
#define tan _safe_tan
#define pow _safe_pow
#define asin _safe_asin
#define acos _safe_acos
)";

static const char k_warp_vs_fx[] = R"(void VS( float3 vPosIn     : POSITION,
         float4 vDiffuseIn : COLOR,
         float2 uv_in      : TEXCOORD0,  // warped UVs (tu, tv)
         float2 uv_orig_in : TEXCOORD1,  // original UVs (tu_orig, tv_orig)
         float2 rad_ang_in : TEXCOORD2,  // .x = rad, .y = ang
     out float4 vPosProj   : POSITION,
     out float4 _vDiffuse  : COLOR,
     out float4 _uv        : TEXCOORD0,  // .xy = warped UVs, .zw = orig UVs
     out float2 _rad_ang   : TEXCOORD1 )
{
    vPosProj = float4(vPosIn.x, vPosIn.y, vPosIn.z, 1);
    _vDiffuse = vDiffuseIn;
    _uv = float4(uv_in.xy, uv_orig_in.xy);
    _rad_ang = rad_ang_in.xy;
}
)";

static const char k_warp_ps_fx[] = R"(shader_body
{
    //************************************************************
    // NOTE: the body of this shader will be replaced by MilkDrop
    //       whenever a pre-MilkDrop-2 preset is loaded!
    //************************************************************

    // sample previous frame
    ret = tex2D( sampler_main, uv ).xyz;

    // darken over time
    ret -= 0.004;

    //************************************************************
    // NOTE: the body of this shader will be replaced by MilkDrop
    //       whenever a pre-MilkDrop-2 preset is loaded!
    //************************************************************
}
)";

static const char k_comp_vs_fx[] = R"(void VS( float3 vPosIn     : POSITION,
         float4 vDiffuseIn : COLOR,
         float2 uv_in      : TEXCOORD0,  // tu, tv (current UVs)
         float2 uv_orig_in : TEXCOORD1,  // tu_orig, tv_orig (original UVs)
         float2 rad_ang_in : TEXCOORD2,  // .x = rad, .y = ang
     out float4 vPosProj   : POSITION,
     out float4 _vDiffuse  : COLOR,
     out float4 _uv        : TEXCOORD0,  // .xy = current UVs, .zw = original UVs
     out float2 _rad_ang   : TEXCOORD1 )
{
    vPosProj = float4(vPosIn.x, vPosIn.y, vPosIn.z, 1);
    _vDiffuse = vDiffuseIn;
    _uv = float4(uv_in.xy, uv_orig_in.xy);
    _rad_ang = rad_ang_in.xy;
})";

static const char k_comp_ps_fx[] = R"(shader_body
{
    //************************************************************
    // NOTE: the body of this shader will be replaced by MilkDrop
    //       whenever a pre-MilkDrop-2 preset is loaded!
    //************************************************************

    ret = tex2D(sampler_main, uv).xyz;

    //************************************************************
    // NOTE: the body of this shader will be replaced by MilkDrop
    //       whenever a pre-MilkDrop-2 preset is loaded!
    //************************************************************
})";

static const char k_blur_vs_fx[] = R"(void VS( float3 vPosIn     : POSITION,
         float4 vDiffuseIn : COLOR,
         float4 uv1        : TEXCOORD0,  // .xy = warped UVs, .zw = orig UVs
         float2 uv2        : TEXCOORD1,  // .x = rad, .y = ang
     out float4 vPosProj   : POSITION,
     out float2 uv         : TEXCOORD0 )
{
    vPosProj = float4(vPosIn.xy,1,1);
    uv = uv1.xy;
}
)";

static const char k_blur1_ps_fx[] = R"(//texture   PrevFrameImage;
//sampler2D sampler_main = sampler_state { Texture = <PrevFrameImage>; };
//float4 _c0; // source texsize (.xy), and inverse (.zw)
//float4 _c1; // w1..w4
//float4 _c2; // d1..d4
//float4 _c3; // scale, bias, w_div




void PS( float2 uv       : TEXCOORD,
     out float4 ret      : COLOR0      )
{
    // LONG HORIZ. PASS 1:
    //const float w[8] = { 4.0, 3.8, 3.5, 2.9, 1.9, 1.2, 0.7, 0.3 };  <- user can specify these
    //const float w1 = w[0] + w[1];
    //const float w2 = w[2] + w[3];
    //const float w3 = w[4] + w[5];
    //const float w4 = w[6] + w[7];
    //const float d1 = 0 + 2*w[1]/w1;
    //const float d2 = 2 + 2*w[3]/w2;
    //const float d3 = 4 + 2*w[5]/w3;
    //const float d4 = 6 + 2*w[7]/w4;
    //const float w_div = 0.5/(w1+w2+w3+w4);
    #define srctexsize _c0
    #define w1 _c1.x
    #define w2 _c1.y
    #define w3 _c1.z
    #define w4 _c1.w
    #define d1 _c2.x
    #define d2 _c2.y
    #define d3 _c2.z
    #define d4 _c2.w
    #define fscale _c3.x
    #define fbias  _c3.y
    #define w_div  _c3.z

    // note: if you just take one sample at exactly uv.xy, you get an avg of 4 pixels.
    // DX12: no half-texel offset needed (pixel centers at integer+0.5).
    // DX9 used float2(1,1) to compensate for half-texel addressing.
    float2 uv2 = uv.xy;

    float3 blur =
            ( tex2D( sampler_main, uv2 + float2( d1*srctexsize.z,0) ).xyz
            + tex2D( sampler_main, uv2 + float2(-d1*srctexsize.z,0) ).xyz)*w1 +
            ( tex2D( sampler_main, uv2 + float2( d2*srctexsize.z,0) ).xyz
            + tex2D( sampler_main, uv2 + float2(-d2*srctexsize.z,0) ).xyz)*w2 +
            ( tex2D( sampler_main, uv2 + float2( d3*srctexsize.z,0) ).xyz
            + tex2D( sampler_main, uv2 + float2(-d3*srctexsize.z,0) ).xyz)*w3 +
            ( tex2D( sampler_main, uv2 + float2( d4*srctexsize.z,0) ).xyz
            + tex2D( sampler_main, uv2 + float2(-d4*srctexsize.z,0) ).xyz)*w4
            ;
    blur.xyz *= w_div;

    blur.xyz = blur.xyz*fscale + fbias;

    ret.xyz = blur;
    ret.w   = 1;
    //ret.xyzw = tex2D(sampler_main, uv + 0*srctexsize.zw);
}
)";

static const char k_blur2_ps_fx[] = R"(//texture   PrevFrameImage;
//sampler2D sampler_main = sampler_state { Texture = <PrevFrameImage>; };
//float4 _c0; // source texsize (.xy), and inverse (.zw)




//float4 _c5; // w1,w2,d1,d2
//float4 _c6; // w_div, edge_darken_c1, edge_darken_c2, edge_darken_c3

void PS( float2 uv       : TEXCOORD,
     out float4 ret      : COLOR0      )
{
    //SHORT VERTICAL PASS 2:
    //const float w1 = w[0]+w[1] + w[2]+w[3];
    //const float w2 = w[4]+w[5] + w[6]+w[7];
    //const float d1 = 0 + 2*((w[2]+w[3])/w1);
    //const float d2 = 2 + 2*((w[6]+w[7])/w2);
    //const float w_div = 1.0/((w1+w2)*2);





    #define srctexsize _c0
    #define w1 _c5.x
    #define w2 _c5.y


    #define d1 _c5.z
    #define d2 _c5.w


    #define edge_darken_c1 _c6.y
    #define edge_darken_c2 _c6.z
    #define edge_darken_c3 _c6.w

    #define w_div   _c6.x

    // note: if you just take one sample at exactly uv.xy, you get an avg of 4 pixels.
    // DX12: no half-texel offset needed (pixel centers at integer+0.5).
    // DX9 used float2(1,0) to compensate for half-texel addressing.
    float2 uv2 = uv.xy;

    float3 blur =
            ( tex2D( sampler_main, uv2 + float2(0, d1*srctexsize.w) ).xyz
            + tex2D( sampler_main, uv2 + float2(0,-d1*srctexsize.w) ).xyz)*w1 +
            ( tex2D( sampler_main, uv2 + float2(0, d2*srctexsize.w) ).xyz
            + tex2D( sampler_main, uv2 + float2(0,-d2*srctexsize.w) ).xyz)*w2
            ;
    blur.xyz *= w_div;

    // tone it down at the edges:  (only happens on 1st X pass!)
    float t = min( min(uv.x, uv.y), 1-max(uv.x,uv.y) );
    t = sqrt(t);
    t = edge_darken_c1 + edge_darken_c2*saturate(t*edge_darken_c3);
    blur.xyz *= t;

    ret.xyz = blur;
    ret.w   = 1;
    //ret.xyzw = tex2D(sampler_main, uv + 0*srctexsize.zw);
}
)";

// Lookup table for embedded shader fallback.
// Filenames include the "data\" prefix to match how ReadFileToString is called.
static const EmbeddedShader k_embedded_shaders[] = {
    { L"data\\include.fx",  k_include_fx },
    { L"data\\warp_vs.fx",  k_warp_vs_fx },
    { L"data\\warp_ps.fx",  k_warp_ps_fx },
    { L"data\\comp_vs.fx",  k_comp_vs_fx },
    { L"data\\comp_ps.fx",  k_comp_ps_fx },
    { L"data\\blur_vs.fx",  k_blur_vs_fx },
    { L"data\\blur1_ps.fx", k_blur1_ps_fx },
    { L"data\\blur2_ps.fx", k_blur2_ps_fx },
};
static const int k_num_embedded_shaders = _countof(k_embedded_shaders);
