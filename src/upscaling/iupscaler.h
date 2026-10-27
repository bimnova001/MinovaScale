#pragma once

#include <cstdint>

/// Common interface for every upscaling method supported by the app.
/// The Phase 1 renderers implement:
///   * XeSSUpscaler  — Intel XeSS-SR (temporal reconstruction)
/// and future milestones can plug in a spatial fallback behind the
/// same contract (see info.md Phase 2).
///
/// The renderer owns command recording; an upscaler only opens
/// resources allocated through the renderer's D3D12 device.
class IUpscaler {
public:
    virtual ~IUpscaler() = default;

    /// Returns the render (input) resolution this upscaler wants, given
    /// the output resolution. Implementations must call this before
    /// allocating scene render targets.
    virtual bool getInputResolution(uint32_t outputW, uint32_t outputH,
                                    uint32_t& inputW,   uint32_t& inputH) = 0;

    /// Number of jitter offsets in the temporal sequence (0 = not temporal).
    virtual uint32_t getJitterSequenceLength() const = 0;

    /// Human-readable label for logs/CSV columns.
    virtual const char* getName() const = 0;
};
