// MinovaUpscaler Phase 2 — AMD FidelityFX Super Resolution 1.0 (FSR 1), EASU.
//
// Edge-Adaptive Spatial Upscaling pass: scales the low-res scene color to the
// output resolution in one pass, then fsr_rcas.hlsl applies RCAS sharpening.
//
// The vendored reference implementation comes from
// third_party/FidelityFX-FSR/ffx-fsr/ffx_a.h + ffx_fsr1.h
// (MIT license, Copyright (c) 2021 Advanced Micro Devices, Inc.).
// This is the 32-bit float path of the AMD sample (its "slow fallback"
// branch) — deliberately not the packed-fp16 fast path, for stability.
//
// Unlike the XeSS path there is no jitter/motion/depth involvement:
// FSR 1 is a spatial upscaler by design.

cbuffer FsrConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
    uint4 Sample;   // unused in EASU; kept for cbuffer layout parity with the sample
};

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D InputTexture : register(t0);
SamplerState linearSampler : register(s0);   // gather-based taps in EASU

// FSR 1 is designed to work on gamma-encoded (sRGB) color: its edge detector
// and RCAS sharpening operate on display-referred values. Two builds:
//   * default (PoC renderer): the scene color is linear HDR, so taps are
//     gamma-encoded on load and RCAS decodes at the end.
//   * with MINOVA_FSR_NO_GAMMA (capture app): the input is already
//     sRGB-encoded desktop color — identity conversion, zero cost.
#ifdef MINOVA_FSR_NO_GAMMA
AF4 MinovaLinearToGammaF(AF4 c) { return c; }
#else
#define MINOVA_GAMMA 2.2
AF4 MinovaLinearToGammaF(AF4 c) { return pow(max(c, AF4_(0.0)), AF4_(1.0 / MINOVA_GAMMA)); }
#endif

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return MinovaLinearToGammaF(InputTexture.GatherRed  (linearSampler, p, int2(0, 0))); }
AF4 FsrEasuGF(AF2 p) { return MinovaLinearToGammaF(InputTexture.GatherGreen(linearSampler, p, int2(0, 0))); }
AF4 FsrEasuBF(AF2 p) { return MinovaLinearToGammaF(InputTexture.GatherBlue (linearSampler, p, int2(0, 0))); }

#include "ffx_fsr1.h"

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
    o.uv.x = (x + 1.0) * 0.5;
    o.uv.y = (1.0 - y) * 0.5;
    return o;
}

float4 psMain(FSQOut i) : SV_Target
{
    // SV_POSITION.xy is (pixel index + 0.5) → uint2 conversion = integer index.
    AU2 ip = AU2(i.pos.xy);
    AF3 c;
    FsrEasuF(c, ip, Const0, Const1, Const2, Const3);
    return float4(c, 1.0);
}
