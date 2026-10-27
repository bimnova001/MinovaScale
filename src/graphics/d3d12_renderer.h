#pragma once

#include "telemetry/gpu_clock.h"
#include "upscaling/fsr1_upscaler.h"
#include "upscaling/xess_upscaler.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

using Microsoft::WRL::ComPtr;   // used by PipelineSet and resource members

struct HWND__;
typedef struct HWND__* HWND;

/// Upscaling mode of the frame.
enum class UpscaleMode {
    Native,     // scene rendered at output resolution — baseline
    XeSS,       // reduced render res + XeSS-SR reconstruction — the goal
    Bilinear,   // reduced render res + plain bilinear upscale — control
    Fsr,        // reduced render res + AMD FSR 1 (EASU+RCAS) spatial upscale
};

const char* UpscaleModeName(UpscaleMode mode);
bool UpscaleModeFromString(const std::string& s, UpscaleMode& out);

/// Minimal D3D12 renderer for the Phase 1 proof of concept.
///
/// Scene: rotating cube on a checkered floor, orbiting camera (animated
/// camera gives non-trivial motion vectors → tests XeSS history/reverbs).
///
/// Per mode:
///   XeSS   : scene at xessGetInputResolution() (with jitter/MVs/depth)
///            -> xessD3D12Execute -> output texture at 1080p -> blit to swap
///   Bilinear : scene at a fixed lower res (no jitter) -> linear blit
///   Native : scene at output resolution (no jitter) -> blit (up scale 1)
///
/// GPU timings via GpuClock: scene_ms & upscale_ms & total, aggregated
/// into a CSV file.
class D3D12Renderer {
public:
    static constexpr UINT kFrameCount = 3;

    D3D12Renderer() = default;
    ~D3D12Renderer() { shutdown(); }
    D3D12Renderer(const D3D12Renderer&) = delete;
    D3D12Renderer& operator=(const D3D12Renderer&) = delete;

    bool initialize(HWND hwnd, UINT outputW, UINT outputH,
                    xess_quality_settings_t quality, int requestedGpuIndex,
                    UpscaleMode mode,
                    const std::string& csvPath,
                    const std::wstring& shaderDir);

    void shutdown();
    void renderFrame();

    void        setMode(UpscaleMode mode) { m_mode = mode; }
    /// RCAS sharpness in stops (0 = max); used next time FSR constants are built.
    void        setFsrSharpness(float stops) { m_fsrSharpness = stops; }
    UpscaleMode getMode() const { return m_mode; }

    UINT   getSceneW() const { return m_sceneW; }
    UINT   getSceneH() const { return m_sceneH; }
    UINT   getOutputW() const { return m_outputW; }
    UINT   getOutputH() const { return m_outputH; }
    double getCpuFps() const { return m_cpuFps; }

    /// true pauses camera/cube animation (jitter keeps advancing).
    bool paused = false;

private:
    struct PipelineSet {
        Microsoft::WRL::ComPtr<ID3D12RootSignature> rootSig;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    };

    struct FrameConstants {
        DirectX::XMFLOAT4X4 currViewProj;   // camera this frame (no jitter)
        DirectX::XMFLOAT4X4 prevViewProj;   // camera last frame
        DirectX::XMFLOAT4X4 currWorld;      // cube world this frame
        DirectX::XMFLOAT4X4 prevWorld;      // cube world last frame
        DirectX::XMFLOAT4 jitter;           // (jx, jy, 0, 0)
        DirectX::XMFLOAT4 resolution;       // (sceneW, sceneH, 0, 0)
    };

    // -- setup ----------------------------------------------
    bool createDeviceAndSwapchain(HWND hwnd, int requestedGpuIndex);
    bool createSyncAndCommandBuffers();
    void createDescriptorHeaps();
    bool createPipelines();
    void createGeometry();
    bool createSceneTargets();
    void releaseSceneTargets();    Microsoft::WRL::ComPtr<ID3DBlob> compileShader(
        const std::wstring& file, const char* entry, const char* target) const;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> buildRootSig(
        const D3D12_ROOT_SIGNATURE_DESC& desc) const;

    // handle helpers
    D3D12_CPU_DESCRIPTOR_HANDLE backBufferRtv(UINT frameIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE sceneRtvSlot(UINT slot) const;  // slot 0=color, 1=velocity
    D3D12_CPU_DESCRIPTOR_HANDLE srvUavSlot(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE srvUavGpuSlot(UINT slot) const;

    // -- frame ------------------------------------------------
    void updateAnimation(double dtSeconds);
    void computeFrameConstants(UINT frameIndex);
    void waitForFence(UINT64 fenceValue);
    bool recordCommandList(UINT frameIndex);

    // -- D3D objects ------------------------------------------
    Microsoft::WRL::ComPtr<IDXGIFactory4>             m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1>             m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3>           m_swapchain;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_rtvHeap;        // backbuffers
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_sceneRtvHeap;   // color + velocity
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>      m_srvUavHeap;     // shader visible

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_cmdAlloc[kFrameCount];
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_cmdList;
    Microsoft::WRL::ComPtr<ID3D12Fence>               m_fence;
    HANDLE                                            m_fenceEvent = nullptr;
    UINT64                                            m_fenceValues[kFrameCount] = {};
    UINT64                                            m_nextFenceValue = 1;

    Microsoft::WRL::ComPtr<ID3D12Resource>            m_renderTargets[kFrameCount];

    Microsoft::WRL::ComPtr<ID3D12Resource>            m_sceneColor;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_sceneVelocity;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_sceneDepth;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_xessOutput;

    // FSR 1 (Phase 2): EASU intermediate + RCAS passes over scene color.
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_fsrEasuOut;     // RGBA8 @ output res
    ComPtr<ID3D12Resource>                            m_fsrConstCb;     // 80 B upload buffer
    D3D12_RESOURCE_STATES                             m_fsrEasuState =
        D3D12_RESOURCE_STATE_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D12Resource>            m_cbUpload[kFrameCount];
    void*                                             m_cbMapped[kFrameCount] = {};

    D3D12_VERTEX_BUFFER_VIEW                          m_vbv = {};
    D3D12_INDEX_BUFFER_VIEW                           m_ibv = {};
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_vertexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_indexBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_vbUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource>            m_ibUpload;
    UINT                                              m_vertexCount = 0;
    UINT                                              m_indexCount = 0;

    PipelineSet                                       m_scenePso;
    PipelineSet                                       m_blitPso;
    PipelineSet                                       m_fsrEasuPso;
    PipelineSet                                       m_fsrRcasPso;

    UINT                                              m_backBufferCount = 0;
    UINT                                              m_rtvSize = 0;
    UINT                                              m_srvSize = 0;
    UINT                                              m_frameIndex = 0;
    UINT64                                            m_frameCounter = 0;

    UINT                                              m_outputW = 0;
    UINT                                              m_outputH = 0;
    UINT                                              m_sceneW = 0;    // render res of scene pass
    UINT                                              m_sceneH = 0;

    UpscaleMode                                       m_mode = UpscaleMode::Native;
    UpscaleMode                                       m_recordedMode = UpscaleMode::Native;
    xess_quality_settings_t                           m_quality = XESS_QUALITY_SETTING_BALANCED;
    float                                             m_fsrSharpness = fsr1::kDefaultSharpnessStops;

    D3D12_RESOURCE_STATES                             m_colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES                             m_velocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES                             m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    D3D12_RESOURCE_STATES                             m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Animation state.
    float               m_cameraTheta = 0.0f;
    float               m_cubeAngle = 0.0f;
    float               m_jitterX = 0.0f;
    float               m_jitterY = 0.0f;
    DirectX::XMFLOAT4X4 m_prevViewProj = {};
    DirectX::XMFLOAT4X4 m_prevWorld = {};

    double m_cpuFps = 0.0;
    double m_lastFrameCpuMs = 0.0;
    std::chrono::steady_clock::time_point m_lastStart{};

    std::unique_ptr<XessUpscaler> m_xess;
    bool                          m_xessReady = false;

    GpuClock    m_gpuClock;
    std::string m_csvPath;
    std::wstring m_shaderDir;
};
