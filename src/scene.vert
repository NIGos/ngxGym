#version 450
// Fullscreen triangle, jittered. Same scene as the D3D11 host so the two halves are
// comparable: if a defect appears on one API and not the other, the scene is not the
// variable.
layout(push_constant) uniform Push {
    vec2 pan;
    vec2 jitter;      // already in clip space
    vec2 inv_render;
    vec2 mv_scale;
} pc;

layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 t = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = t;
    gl_Position = vec4(t * 2.0 - 1.0, 0.0, 1.0);
    gl_Position.xy += pc.jitter;
}
