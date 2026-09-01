#version 450
layout(push_constant) uniform Push {
    vec2 pan;
    vec2 jitter;
    vec2 inv_render;
    vec2 mv_texel;
} pc;

layout(location = 0) in  vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec2 o_mv;

float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1, 0)), f.x),
               mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), f.x), f.y);
}

void main()
{
    vec2  p   = v_uv / pc.inv_render + pc.pan;
    float chk = step(0.5, fract(floor(p.x / 24.0) * 0.5 + floor(p.y / 24.0) * 0.5));
    float n   = noise(p * 0.08) * 0.6 + noise(p * 0.31) * 0.3;
    o_color = vec4(clamp(vec3(chk * 0.8 + n, n, 1.0 - chk * 0.6) * 1.4, 0.0, 1.0), 1.0);

    // Where this pixel was, minus where it is: a constant field. Precomputed on the
    // CPU as velocity/MV.Scale so the value here is the texel NGX will read, not an
    // expression that has to be got right in two places -- see the note in the host.
    o_mv = pc.mv_texel;

    gl_FragDepth = clamp(0.2 + 0.6 * v_uv.y, 0.0, 1.0);
}
