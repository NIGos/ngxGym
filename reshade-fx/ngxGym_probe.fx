// The smallest ReShade effect that is still an effect.
//
// It exists for one reason: reshade_begin_effects only fires when ReShade actually
// runs something, and the dlss5-bridge add-on's source-latch release needs thirty of
// those ticks. A run folder with no effects enabled therefore never releases the
// latch -- which is what a user reported in Baldur's Gate 3 on D3D11 and what could
// not be got into a log there. With this enabled the tick happens, and the scenario
// answers the question either way.
//
// It writes the back buffer back unchanged, so it cannot be mistaken for the cause
// of anything visual.
#include "ReShade.fxh"

float4 PS_Passthrough(float4 pos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(ReShade::BackBuffer, uv);
}

technique ngxGym_probe
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Passthrough;
    }
}
