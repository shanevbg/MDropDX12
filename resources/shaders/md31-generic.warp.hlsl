shader_body
{
    // Generic warp for the MilkDrop2077 / MD31 family.
    //
    // 53 of the 54 presets in MD3's presets1 carrying an MD31 or MD32 key ship
    // a STUB warp shader -- typically just a sample of the previous frame and a
    // decay -- because MilkDrop 3 PRO renders their real one out of its own
    // cache instead.  Anything that is not MD3 PRO therefore renders a nearly
    // static frame.  This is a deliberately mild general-purpose replacement:
    // enough motion to make the family alive, gentle enough not to fight the
    // per-frame and wave code, which is present and correct in every one.
    //
    // Presets wanting their own look get a more specific rule ABOVE the md31
    // rule in shaderoverrides.json -- first match wins.
    //
    // Original work. It reproduces no MD3 PRO shader.

    float2 c = float2(0.5, 0.5);
    float2 d = (uv - c) * float2(aspect.x, 1.0);

    float r = length(d) + 1e-6;
    // atan2(0,0) is NaN under DX12's stricter IEEE handling -- bias x.
    float a = atan2(d.y, d.x + 1e-20);

    // A swirl that grows with radius, breathing on the bass.
    a  += 0.06 * sin(time * 0.13) + 0.05 * r * (0.5 + 0.5 * bass_att);
    float rr = r * (0.995 - 0.006 * bass_att);

    float2 suv = c + float2(cos(a), sin(a)) * rr / float2(aspect.x, 1.0);
    ret = tex2D(sampler_main, suv).xyz;

    float  h    = frac(r * 0.6 - time * 0.02);
    float3 tint = 0.5 + 0.5 * cos(6.2831853 * (h + float3(0.0, 0.33, 0.67)));
    ret = lerp(ret, ret * tint * 1.12, 0.16);

    ret *= 0.975;
}
