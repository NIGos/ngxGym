// A texMotionVectors provider that writes zero motion, and a depth tap.
//
// Under Proton the driver's optical flow does not open (the session is refused
// with INVALID_PTR), so the substitute contract falls back to the motion-vector
// texture a ReShade effect provides -- and a run folder with no such effect can
// never arm there. This is the smallest provider there is. Zero motion is wrong
// for a moving scene and right for this purpose: the scenarios measure whether
// the contract arms, rebuilds and delivers, not what the picture looks like.
//
// The depth read is deliberate as well: DepthBufferTex is bound only once some
// loaded effect uses the DEPTH semantic, and the probe does not. The comparison
// can never be true (linearised depth is at most 1), so the output stays zero
// while the read cannot be folded away. Contributed by flshy1337 (dlss5-bridge
// #22), under the name MVStub.fx.
#include "ReShade.fxh"

texture texMotionVectors < pooled = false; > { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };
sampler SamplerMotionVectors { Texture = texMotionVectors; };

float2 PS_Zero(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    const float d = ReShade::GetLinearizedDepth(uv);
    return d > 2.0 ? float2(1.0, 1.0) : float2(0.0, 0.0);
}

technique ngxGym_mv
{
    pass { VertexShader = PostProcessVS; PixelShader = PS_Zero; RenderTarget = texMotionVectors; }
}
