#pragma once

#include "iupscaler.h"
#include "xess.h"
#include "xess_d3d12.h"

#include <cstdint>

/// Parses "ultra_performance|performance|balanced|quality|ultra_quality|..."
xess_quality_settings_t parseXessQuality(const char* name);

/// Wrapper around the Intel XeSS-SR API (D3D12 backend).
///
/// XeSS-SR is a *temporal* upscaler: per frame it needs jitter, low-res
/// motion vectors and depth (this wrapper always uses the low-res MV +
/// depth path, i.e. XeSS dilates internally) and produces a target-res
/// output texture.
class XessUpscaler : public IUpscaler {
public:
    ~XessUpscaler() override;

    /// Version of the loaded libxess.dll ("major.minor.patch").
    static const char* getVersionString();

    /// Creates the context and initializes XeSS for the given output
    /// resolution. Must be called exactly once, before the first frame.
    /// Throws std::runtime_error on failure.
    void initialize(ID3D12Device* device,
                    uint32_t outputW, uint32_t outputH,
                    xess_quality_settings_t quality);

    void shutdown();

    bool getInputResolution(uint32_t outputW, uint32_t outputH,
                            uint32_t& inputW, uint32_t& inputH) override;
    uint32_t getJitterSequenceLength() const override { return 32; }
    const char* getName() const override { return "xess-sr"; }

    void setQuality(xess_quality_settings_t quality) { m_quality = quality; }

    /// Thin wrapper over xessD3D12Execute so the renderer does not need the
    /// SDK header directly. Jitter values must be the same values used to
    /// jitter the camera this frame, as passed here (SDK convention).
    bool upscale(ID3D12GraphicsCommandList* cmdList,
                 ID3D12Resource* color, ID3D12Resource* velocity,
                 ID3D12Resource* depth, ID3D12Resource* output,
                 float jitterX, float jitterY,
                 uint32_t inputW, uint32_t inputH);

    /// Reports loaded versions and driver status to stdout.
    void logStatus() const;

private:
    xess_context_handle_t   m_context     = nullptr;
    xess_quality_settings_t m_quality     = XESS_QUALITY_SETTING_BALANCED;
    xess_2d_t               m_outputRes{};
    xess_2d_t               m_inputRes{};
};
