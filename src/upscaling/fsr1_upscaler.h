#pragma once

// AMD FidelityFX Super Resolution 1.0 (FSR 1) — spatial upscaling fallback.
//
// Phase 2 of the mission (per info.md): a second upscaler behind the same
// quality-driven controls, useful on hardware/driver combinations where the
// temporal path (XeSS) is not available. FSR 1 is purely spatial (EASU lane
// adaptation + RCAS sharpening): no jitter/motion vectors/depth are needed,
// so it must be honestly labeled — same render-res reduction, but the
// reconstruction quality is below a temporal upscaler's on animated scenes.
//
// Vendored reference implementation: third_party/FidelityFX-FSR
// (ffx_a.h + ffx_fsr1.h, MIT license, (c) 2021 Advanced Micro Devices, Inc.).
// third_party/ is never modified; we only compile against it and copy the
// two headers next to our shaders at build time.

#include <cstdint>

#include "xess.h"   // xess_quality_settings_t (scale table parity with XeSS)

namespace fsr1 {

/// Internal-resolution scale ratio for a quality preset. These mirror the
/// documented XeSS-SR preset table so both upscalers can be tested at exactly
/// the same render-resolution reduction.
float scaleRatioFor(xess_quality_settings_t quality);

/// Default RCAS sharpness in "stops of sharpness reduction"
/// (0.0 = maximum sharpening; higher = softer). 0.2 is close to AMD's
/// sample default without the noisiest endpoint.
constexpr float kDefaultSharpnessStops = 0.2f;

/// Constant buffer layout shared by fsr_easu.hlsl and fsr_rcas.hlsl —
/// identical to the AMD sample's cbuffer (uint4 Const0..3 + uint4 Sample).
struct alignas(16) Constants {
    std::uint32_t c[5][4];
};

/// CPU-side constant setup (compiles ffx_a.h/ffx_fsr1.h with A_CPU):
/// fills Const0..3 via FsrEasuCon and Const0-style RCAS via FsrRcasCon
/// (stored in c[4]).
void computeConstants(unsigned inputW, unsigned inputH,
                      unsigned outputW, unsigned outputH,
                      float sharpnessStops, Constants* out);

/// Same, with a pixel crop offset into the input texture (EasuConOffset) —
/// used by the capture app to upscale only the client area of a window.
void computeConstantsCrop(unsigned inputViewportW, unsigned inputViewportH,
                          unsigned inputImageW, unsigned inputImageH,
                          unsigned outputW, unsigned outputH,
                          unsigned offsetX, unsigned offsetY,
                          float sharpnessStops, Constants* out);

} // namespace fsr1
