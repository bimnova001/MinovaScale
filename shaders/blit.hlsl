// MinovaUpscaler Phase 1 — fullscreen blit / upscaling presentation pass.
//
// A big triangle covering the viewport; samples a source texture (either
// the 720p scene color for the "bilinear" control group and the native
// mode, or the XeSS output texture). With a linear sampler, this pass
// acts as the classic bilinear upscaler; XeSS output goes through the
// same path for a pixel-aligned copy.

struct FSQOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

FSQOut vsMain(uint vid : SV_VertexID)
{
    FSQOut o;
    // Big triangle: (-1,-1) (3,-1) (-1,3) covers the viewport.
    float x = (vid == 1) ? 3.0 : -1.0;
    float y = (vid == 2) ? 3.0 : -1.0;
    o.pos = float4(x, y, 0.0, 1.0);
    o.uv.x = (x + 1.0) * 0.5;     // [0 .. 2 ] across the screen
    o.uv.y = (1.0 - y) * 0.5;
    return o;
}

Texture2D srcTexture : register(t0);
SamplerState linearSampler : register(s0);

float4 psMain(FSQOut i) : SV_Target
{
    return srcTexture.Sample(linearSampler, i.uv);
}
