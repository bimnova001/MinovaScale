#include "fsr1_upscaler.h"

#include <cstring>
#include <float.h>
#include <math.h>
#include <stdint.h>

// CPU side of the vendored FSR 1 reference implementation (MIT, AMD).
// A_CPU makes ffx_a.h/ffx_fsr1.h compile as portable C++ and exposes
// FsrEasuCon/FsrRcasCon constant-setup functions. ffx_a.h uses the global
// math functions, hence the C headers above.
#define A_CPU 1
#include <ffx_a.h>
#include <ffx_fsr1.h>

namespace fsr1 {

float scaleRatioFor(xess_quality_settings_t quality)
{
    // XeSS-SR developer guide "Preset" table (resolution-scaling ratios).
    switch (quality) {
    case XESS_QUALITY_SETTING_ULTRA_PERFORMANCE: return 3.0f;
    case XESS_QUALITY_SETTING_PERFORMANCE:       return 2.3f;
    case XESS_QUALITY_SETTING_BALANCED:          return 2.0f;
    case XESS_QUALITY_SETTING_QUALITY:           return 1.7f;
    case XESS_QUALITY_SETTING_ULTRA_QUALITY:     return 1.5f;
    case XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS:return 1.3f;
    case XESS_QUALITY_SETTING_AA:                return 1.0f;
    default:                                     return 2.0f;
    }
}

void computeConstants(unsigned inputW, unsigned inputH,
                      unsigned outputW, unsigned outputH,
                      float sharpnessStops, Constants* out)
{
    static_assert(sizeof(Constants) == 80, "cbuffer layout parity with the shaders");

    AU1 e0[4], e1[4], e2[4], e3[4];
    FsrEasuCon(e0, e1, e2, e3,
               AF1_(float(inputW)),  AF1_(float(inputH)),   // input viewport (whole image)
               AF1_(float(inputW)),  AF1_(float(inputH)),   // input image size
               AF1_(float(outputW)), AF1_(float(outputH)));

    AU1 r[4];
    FsrRcasCon(r, AF1_(sharpnessStops));

    std::memcpy(out->c[0], e0, 4 * sizeof(AU1));
    std::memcpy(out->c[1], e1, 4 * sizeof(AU1));
    std::memcpy(out->c[2], e2, 4 * sizeof(AU1));
    std::memcpy(out->c[3], e3, 4 * sizeof(AU1));
    std::memcpy(out->c[4], r,  4 * sizeof(AU1));   // Sample slot: RCAS reuses Const0
}

void computeConstantsCrop(unsigned inputViewportW, unsigned inputViewportH,
                          unsigned inputImageW, unsigned inputImageH,
                          unsigned outputW, unsigned outputH,
                          unsigned offsetX, unsigned offsetY,
                          float sharpnessStops, Constants* out)
{
    AU1 e0[4], e1[4], e2[4], e3[4];
    FsrEasuConOffset(e0, e1, e2, e3,
                     AF1_(float(inputViewportW)), AF1_(float(inputViewportH)),
                     AF1_(float(inputImageW)),    AF1_(float(inputImageH)),
                     AF1_(float(outputW)),        AF1_(float(outputH)),
                     AF1_(float(offsetX)),        AF1_(float(offsetY)));

    AU1 r[4];
    FsrRcasCon(r, AF1_(sharpnessStops));

    std::memcpy(out->c[0], e0, 4 * sizeof(AU1));
    std::memcpy(out->c[1], e1, 4 * sizeof(AU1));
    std::memcpy(out->c[2], e2, 4 * sizeof(AU1));
    std::memcpy(out->c[3], e3, 4 * sizeof(AU1));
    std::memcpy(out->c[4], r,  4 * sizeof(AU1));
}

} // namespace fsr1
