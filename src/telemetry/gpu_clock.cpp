#include "gpu_clock.h"

#include <cstdio>
#include <cstring>

#include <d3d12.h>
// Command queue is needed only for the timestamp frequency query.
#include <dxgi.h>

bool GpuClock::initialize(ID3D12Device* device)
{
    m_device = device;

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type     = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count    = kRingFrames * kSlotsPerFrame;
    heapDesc.NodeMask = 0;
    HRESULT hr = device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(m_queryHeap.GetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[telemetry] CreateQueryHeap failed 0x%08lX\n", (unsigned long)hr);
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC rbDesc = {};
    rbDesc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Width              = sizeof(UINT64) * kRingFrames * kSlotsPerFrame;
    rbDesc.Height             = 1;
    rbDesc.DepthOrArraySize   = 1;
    rbDesc.MipLevels          = 1;
    rbDesc.Format             = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc.Count   = 1;
    rbDesc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE,
        &rbDesc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(m_readback.GetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "[telemetry] readback resource failed 0x%08lX\n", (unsigned long)hr);
        return false;
    }

    // Keep the readback buffer persistently mapped.
    D3D12_RANGE zeroRange = {0, 0};
    hr = m_readback->Map(0, &zeroRange, &m_mappedData);
    if (FAILED(hr)) {
        std::fprintf(stderr, "[telemetry] Map(readback) failed 0x%08lX\n", (unsigned long)hr);
        return false;
    }

    m_initialized = true;
    return true;
}
void GpuClock::shutdown()
{
    if (m_readback && m_mappedData) {
        D3D12_RANGE written = {0, 0};
        m_readback->Unmap(0, &written);
        m_mappedData = nullptr;
    }
    m_queryHeap.Reset();
    m_readback.Reset();
    m_device = nullptr;
    m_initialized = false;
}

void GpuClock::setFrequency(UINT64 frequency)
{
    m_frequency = (frequency > 0) ? frequency : 1;
}

void GpuClock::recordSceneStart(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                      m_writeIndex * kSlotsPerFrame + 0);
}

void GpuClock::recordSceneEnd(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                      m_writeIndex * kSlotsPerFrame + 1);
}

void GpuClock::recordUpscaleEnd(ID3D12GraphicsCommandList* cmdList)
{
    cmdList->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                      m_writeIndex * kSlotsPerFrame + 2);
}

void GpuClock::resolve(ID3D12GraphicsCommandList* cmdList)
{
    UINT64 dst = m_writeIndex * kSlotsPerFrame;
    cmdList->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
                              dst, kSlotsPerFrame, m_readback.Get(), dst * sizeof(UINT64));
}

void GpuClock::beginFrame(const std::string& csvPath, const char* modeLabel,
                          double cpuFps)
{
    if (!m_initialized)
        return;

    if (m_counter >= kHarvestDelay)
        harvest(csvPath, modeLabel, cpuFps);

    m_writeIndex = (m_writeIndex + 1) % kRingFrames;
    m_counter++;
}

void GpuClock::harvest(const std::string& csvPath, const char* modeLabel,
                       double cpuFps)
{
    // Frame F writes slot (F+1) % kRingFrames (beginFrame advances
    // m_writeIndex before the frame records, with counters starting at 0).
    // beginFrame(C) harvests frame C - kHarvestDelay, whose slot is:
    UINT harvestIdx = (m_writeIndex + 1 + kRingFrames - kHarvestDelay) % kRingFrames;

    UINT64 ticks[3];
    const UINT64* src =
        reinterpret_cast<const UINT64*>(m_mappedData) + harvestIdx * kSlotsPerFrame;
    std::memcpy(ticks, src, sizeof(ticks));

    double scaleMs = 1000.0 / static_cast<double>(m_frequency);
    double sceneMs = (ticks[1] > ticks[0]) ? (ticks[1] - ticks[0]) * scaleMs : 0.0;
    double upsMs   = (ticks[2] > ticks[1]) ? (ticks[2] - ticks[1]) * scaleMs : 0.0;
    double totalMs = (ticks[2] > ticks[0]) ? (ticks[2] - ticks[0]) * scaleMs : 0.0;

    m_sumSceneMs += sceneMs;
    m_sumUpsMs   += upsMs;
    m_sumTotalMs += totalMs;
    m_pending++;

    if (m_pending >= kAggFrames) {
        FILE* f = std::fopen(csvPath.c_str(), "a");
        if (!f) {
            static bool warned = false;
            if (!warned) {
                std::fprintf(stderr, "[telemetry] cannot open %s for writing\n", csvPath.c_str());
                warned = true;
            }
        } else {
            // Write header once.
            std::fseek(f, 0, SEEK_END);
            if (std::ftell(f) == 0)
                std::fprintf(f, "window_end_frame,mode,avg_gpu_scene_ms,avg_gpu_upscale_ms,avg_gpu_total_ms,cpu_fps\n");
            std::fprintf(f, "%u,%s,%.3f,%.3f,%.3f,%.1f\n",
                         m_counter, modeLabel,
                         m_sumSceneMs / kAggFrames,
                         m_sumUpsMs / kAggFrames,
                         m_sumTotalMs / kAggFrames,
                         cpuFps);
            std::fclose(f);
        }

        m_pending    = 0;
        m_sumSceneMs = m_sumUpsMs = m_sumTotalMs = 0.0;
    }
}
