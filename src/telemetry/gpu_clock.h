#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

/// D3D12 GPU timestamp helper.
///
/// Measures two GPU segments per frame, recorded inside the frame's own
/// command list:
///
///     T0 : start of scene pass
///     T1 : end of scene pass       -> scene_ms      = T1 - T0
///     T2 : end of upscale pass     -> upscale_ms    = T2 - T1
///                                  -> total_gpu_ms  = T2 - T0
///
/// Timestamps live in a query heap; each frame resolves its own 3 slots
/// into a small readback ring buffer. The ring is read kHarvestDelay
/// frames later, when the GPU work that wrote it is guaranteed retired
/// (the app waits for the fence value covering 3-frames-ago each frame).
///
/// Every kAggFrames harvested frames, one aggregated CSV row is appended.
class GpuClock {
public:
    /// Call once, after device creation.
    bool initialize(ID3D12Device* device);

    void shutdown();

    /// Call once per frame on the CPU, after the fence wait. Advances the
    /// ring and (from frame kHarvestDelay on) harvests that old frame's
    /// timestamps into the aggregation window, possibly appending a CSV row.
    void beginFrame(const std::string& csvPath, const char* modeLabel,
                    double cpuFps);

    // --- recorded inside the frame's command list, in this order ---
    void      recordSceneStart(ID3D12GraphicsCommandList* cmdList);
    void      recordSceneEnd(ID3D12GraphicsCommandList* cmdList);
    void      recordUpscaleEnd(ID3D12GraphicsCommandList* cmdList);
    /// Copies this frame's 3 timestamps to the readback ring.
    void      resolve(ID3D12GraphicsCommandList* cmdList);
    // --------------------------------------------------------------

    /// GPU tick frequency, from
    /// ID3D12CommandQueue::GetTimestampFrequency after queue creation.
    void      setFrequency(UINT64 frequency);

private:
    static const UINT kSlotsPerFrame = 3;    // T0 T1 T2
    static const UINT kRingFrames    = 8;    // frames of timestamp storage
    static const UINT kHarvestDelay  = 3;    // frames before readback
    static const UINT kAggFrames     = 60;   // frames per CSV row

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_queryHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource>  m_readback;
    ID3D12Device* m_device = nullptr;
    void*         m_mappedData = nullptr;
    bool          m_initialized = false;


    UINT64 m_frequency = 1;
    UINT   m_writeIndex = 0;            // ring index for the frame being recorded
    UINT   m_counter    = 0;            // total frames counter

    UINT   m_pending   = 0;             // frames accumulated in CSV window
    double m_sumSceneMs = 0.0;
    double m_sumUpsMs   = 0.0;
    double m_sumTotalMs = 0.0;

    void harvest(const std::string& csvPath, const char* modeLabel, double cpuFps);
};
