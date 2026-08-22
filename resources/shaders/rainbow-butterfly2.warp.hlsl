shader_body
{
    // Rainbow Butterfly 2 -- kaleidoscope tunnel.
    //
    // Butterfly2 is byte-identical to Butterfly1 apart from two comment lines
    // and its MD31 value, and MD3 PRO resolves that value into a DIFFERENT
    // cache file (shaders4 versus shaders3).  So the pair is meant to look
    // different, and this override is deliberately not a variation on
    // rainbow-butterfly1.warp.hlsl -- it is a separate effect.
    //
    // Original work; see rainbow-butterfly1.warp.hlsl for the provenance note.

    float2 c = float2(q10, q11);
    float2 d = (uv - c) * float2(aspect.x, 1.0);

    float r = length(d) + 1e-6;
    // atan2(0,0) is NaN under DX12's stricter IEEE handling -- bias x.
    float a = atan2(d.y, d.x + 1e-20);

    // Fold the angle into N wedges.  q3 is the preset's gmegabuf counter, so
    // the wedge count moves with the preset's own state rather than the clock.
    float N = 4.0 + floor(q3 * 2.0);
    float w = 6.2831853 / N;
    a = abs(frac(a / w + 0.5) - 0.5) * w;

    // A reciprocal term pulls the centre outward: this is what reads as depth.
    float rr = r * (1.0 + 0.06 * q9) - 0.004 * q1 / (r + 0.25);

    float2 suv = c + float2(cos(a), sin(a)) * rr / float2(aspect.x, 1.0);
    ret = tex2D(sampler_main, suv).xyz;

    float  h    = frac(a / 6.2831853 + rr * 0.8 - time * 0.02);
    float3 tint = 0.5 + 0.5 * cos(6.2831853 * (h + float3(0.0, 0.33, 0.67)));
    ret = lerp(ret, ret * tint * 1.2, 0.26);

    ret *= 0.962;
}
