shader_body
{
    // Rainbow Butterfly 1 -- wing fold.
    //
    // The preset this attaches to ships a stub warp shader (a sample of the
    // previous frame and ret *= 0.94) because its real one lives in MilkDrop 3
    // PRO's cache, selected by its MD31 key.  This is an original replacement,
    // not a reproduction: it takes the recovered shader's structure -- an
    // axis fold, a slow rotation and a frac() rainbow -- and nothing else.
    //
    // It drives off the values the preset's own per_frame code exports:
    //   q1  pvol          q8, q9  slow oscillators     q10, q11  centre
    //   q12 bal           q15     0.5 - 0.35*tan(t1)

    float2 c = float2(q10, q11);
    float2 d = (uv - c) * float2(aspect.x, 1.0);

    // Butterfly symmetry: reflect the half-plane behind a slowly tilting axis
    // onto the half in front of it, so the two wings mirror.
    float  tilt = q8 * 6.2831853;
    float2 ax   = float2(cos(tilt), sin(tilt));
    d -= ax * min(dot(d, ax), 0.0) * 2.0;

    // Rotate with the stereo balance, breathe with volume.
    float rot = (q12 - 0.5) * 0.6 + q9 * 3.14159265;
    float cs = cos(rot), sn = sin(rot);
    d = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) * (1.0 - 0.012 * q1);

    float2 suv = c + d / float2(aspect.x, 1.0);

    // Two taps across the fold give the wing edge its layering.
    float3 a = tex2D(sampler_main, suv).xyz;
    float3 b = tex2D(sampler_main, lerp(suv, uv, 0.35)).xyz;
    ret = lerp(a, b, 0.35);

    // Drift the hue with radius and time rather than decaying towards grey.
    float  h    = frac(rad * 0.5 + time * 0.03 + q15);
    float3 tint = 0.5 + 0.5 * cos(6.2831853 * (h + float3(0.0, 0.33, 0.67)));
    ret = lerp(ret, ret * tint * 1.15, 0.22);

    ret *= 0.965 - 0.02 * q1;
}
