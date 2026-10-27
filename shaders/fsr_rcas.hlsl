// MinovaUpscaler Phase 2 — AMD FidelityFX Super Resolution 1.0 (FSR 1), RCAS.
//
// Robust Contrast-Adaptive Sharpening over the EASU-upscaled intermediate,
// writing straight to the swap-chain back buffer.
//
// Vendored reference implementation from
// third_party/FidelityFX-FSR/ffx-fsr/ffx_a.h + ffx_fsr1.h
// (MIT license, Copyright (c) 2021 Advanced Micro Devices, Inc.),
// 32-bit float path (the AMD sample's "slow fallback" branch).
//
// The RCAS sharpness constant is Const0 (packed by FsrRcasCon on the CPU).

cbuffer FsrConstants : register(b0)
{
    uint4 Const0;
    uint4 Const1;
    uint4 Const2;
    uint4 Const3;
    uint4 Sample;   // unused in RCAS; kept for cbuffer layout parity with the sample
};

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

Texture2D InputTexture : register(t0);

// Match fsr_easu.hlsl: EASU wrote gamma-encoded values into the intermediate
// (renderer build) or identity (capture build); RCAS sharpens accordingly and
// the output is what each mode's presentation expects.
#ifdef MINOVA_FSR_NO_GAMMA
AF4 MinovaGammaToLinearF(AF4 c) { return c; }
#else
#define MINOVA_GAMMA 2.2
AF4 MinovaGammaToLinearF(AF4 c) { return pow(max(c, AF4_(0.0)), AF4_(MINOVA_GAMMA)); }
#endif

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(ASU2(p), 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

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
    AU2 ip = AU2(i.pos.xy);
    AF1 r, g, b;
    FsrRcasF(r, g, b, ip, Const0);
    // RCAS ran in gamma space; decode back to linear so the output matches
    // the other modes' appearance (they write linear values directly).
    return MinovaGammaToLinearF(float4(r, g, b, 1.0));
}
