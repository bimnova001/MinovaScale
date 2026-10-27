// MinovaUpscaler Phase 1 — scene shaders.
//
// One pass renders a rotating cube on a checkered floor into TWO render
// targets:
//   target0 : HDR color (R16G16B16A16_FLOAT), linear scene-referred values
//   target1 : motion vectors (R16G16_FLOAT) in NDC units, encoded as
//             prevNDC - currNDC ("motion from current frame to previous
//             frame", per the XeSS-SR developer guide) with the
//             XESS_INIT_FLAG_USE_NDC_VELOCITY convention.
// Depth comes from the depth-stencil view (D32_FLOAT, standard depth:
// smaller = closer, XeSS default — no INVERTED_DEPTH flag).
//
// Camera/cube are unjittered here; when XeSS is active the CPU uploads
// non-zero jitter and the vertex shader displaces the rasterized clip
// position by the subpixel offset ( ClipXY += W * (jx*2/W, -jy*2/H) ) —
// motion vectors are computed from the UNJITTERED positions, matching
// the XeSS requirement that MVs exclude camera jitter.

cbuffer FrameConstants : register(b0)
{
    row_major float4x4 currViewProj;  // camera this frame (no jitter)
    row_major float4x4 prevViewProj;  // camera previous frame (no jitter)
    row_major float4x4 currWorld;     // cube world matrix, this frame
    row_major float4x4 prevWorld;     // cube world matrix, previous frame
    float4 jitter;                    // (jx, jy) subpixel offsets in [-0.5, 0.5]
    float4 resolution;                // (sceneW, sceneH, 0, 0)
};

struct VSIn
{
    float3 pos    : POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD;
    uint  isCube  : BLENDINDICES;    // 1 = rotating cube (model space), 0 = baked world-space floor
};

struct VSOut
{
    float4 pos : SV_POSITION;
    noperspective float4 currNdc : TEX0;    // unjittered current clip position
    noperspective float4 prevNdc : TEX1;    // previous-frame clip position
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD2;
    nointerpolation uint isCube : BLENDINDICES;
};

VSOut vsMain(VSIn vin)
{
    VSOut o;

    float4 wpos;
    float4 pwpos;
    float3 wnormal;

    if (vin.isCube != 0u) {
        wpos    = mul(currWorld, float4(vin.pos, 1.0));
        pwpos   = mul(prevWorld, float4(vin.pos, 1.0));
        wnormal = mul((float3x3)currWorld, vin.normal);
    } else {
        // Floor vertices are already in world space; the floor never moves.
        wpos    = float4(vin.pos, 1.0);
        pwpos   = wpos;
        wnormal = vin.normal;
    }

    float4 unjitteredClip = mul(currViewProj, wpos);

    // Rasterized position: add the camera jitter (for XeSS mode).
    float4 jitteredClip = unjitteredClip;
    jitteredClip.x += jitteredClip.w * (jitter.x * 2.0 / resolution.x);
    jitteredClip.y -= jitteredClip.w * (jitter.y * 2.0 / resolution.y);

    o.pos         = jitteredClip;
    o.currNdc     = unjitteredClip;
    o.prevNdc     = mul(prevViewProj, pwpos);
    o.worldNormal = wnormal;
    o.uv          = vin.uv;
    o.isCube      = vin.isCube;
    return o;
}

struct PSOut
{
    float4 color : SV_Target0;   // HDR / scene-referred linear
    float2 mv    : SV_Target1;   // NDC motion vectors (prev - curr)
};

static const float3 s_lightDir = float3(0.35, 0.80, 0.45);  // normalized below
static const float3 s_cubeColor = float3(0.82, 0.30, 0.18);

PSOut psMain(VSOut i)
{
    PSOut o;

    float3 n = normalize(i.worldNormal);
    float3 l = normalize(s_lightDir);
    float lambert = max(dot(n, l), 0.0);

    if (i.isCube != 0u) {
        o.color = float4(s_cubeColor * (0.12 + 0.88 * lambert), 1.0);
    } else {
        // Checker floor in world space (uv encodes world XZ * scale).
        float2 cell = floor(i.uv * 4.0);
        float odd   = fmod(cell.x + cell.y, 2.0);
        float3 base = (odd < 0.5) ? float3(0.60, 0.63, 0.66)
                                  : float3(0.24, 0.26, 0.30);
        o.color = float4(base * (0.05 + 0.95 * lambert), 1.0);
    }

    float2 curr = i.currNdc.xy / i.currNdc.w;   // this frame's NDC position
    float2 prev = i.prevNdc.xy / i.prevNdc.w;   // previous frame's position
    o.mv = prev - curr;
    return o;
}
