#include "d3d12_renderer.h"

#include "intel/adapter_selector.h"
#include "upscaling/fsr1_upscaler.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

// Present in newer SDK headers; keep identical definition as fallback.
#ifndef D3D_COMPILE_STANDARD_FILE_INCLUDES
#define D3D_COMPILE_STANDARD_FILE_INCLUDES ((ID3DInclude*)(UINT_PTR)1)
#endif

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// mode helpers (declared in the header, used by the app as well)
// ---------------------------------------------------------------------------
namespace {

const char* modeName(UpscaleMode mode)
{
    switch (mode) {
    case UpscaleMode::Native:   return "native";
    case UpscaleMode::XeSS:     return "xess";
    case UpscaleMode::Bilinear: return "bilinear";
    case UpscaleMode::Fsr:      return "fsr";
    }
    return "?";
}

bool parseMode(const std::string& s, UpscaleMode& out)
{
    if (s == "native")   { out = UpscaleMode::Native;   return true; }
    if (s == "xess")     { out = UpscaleMode::XeSS;     return true; }
    if (s == "bilinear") { out = UpscaleMode::Bilinear; return true; }
    if (s == "fsr")      { out = UpscaleMode::Fsr;      return true; }
    return false;
}

/// Halton low-discrepancy sequence: value 1..n in [0, 1).
float haltonSample(UINT index, UINT base)
{
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "%s failed (HRESULT 0x%08lX)",
                      what, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

void ThrowIfFailedW(HRESULT hr, const wchar_t* what)
{
    if (FAILED(hr)) {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "resource \"%ls\" failed (HRESULT 0x%08lX)",
                      what, static_cast<unsigned long>(hr));
        throw std::runtime_error(msg);
    }
}

#if defined(_DEBUG)
void CALLBACK d3d12DebugCallback(D3D12_MESSAGE_CATEGORY,
                                 D3D12_MESSAGE_SEVERITY severity,
                                 D3D12_MESSAGE_ID,
                                 LPCSTR desc, void*)
{
    const char* tag = "INFO";
    switch (severity) {
    case D3D12_MESSAGE_SEVERITY_WARNING:      tag = "WARN"; break;
    case D3D12_MESSAGE_SEVERITY_ERROR:        tag = "ERROR"; break;
    case D3D12_MESSAGE_SEVERITY_CORRUPTION:   tag = "CORRUPT"; break;
    default: break;
    }
    std::fprintf(stderr, "[d3d12][%s] %s\n", tag, desc);
}
#endif

struct GeoVertex {
    XMFLOAT3 pos;
    XMFLOAT3 normal;
    XMFLOAT2 uv;
    UINT     isCube;
};

constexpr UINT kCubeVerts    = 24;
constexpr UINT kFloorVerts   = 4;
constexpr UINT kTotalVerts   = kCubeVerts + kFloorVerts;
constexpr UINT kTotalIndices = 36 + 6;

void fillGeometry(GeoVertex* v, UINT16* idx)
{
    UINT x = 0;

    auto push = [&](float px, float py, float pz,
                    float nx, float ny, float nz, float u, float vv) {
        v[x++] = {{px, py, pz}, {nx, ny, nz}, {u, vv}, 1u};
    };

    // faces of the cube: 24 verts (4 per face), CCW seen from outside
    push(+0.5f, -0.5f, -0.5f,   1, 0, 0,   0, 0);
    push(+0.5f, +0.5f, -0.5f,   1, 0, 0,   0, 1);
    push(+0.5f, +0.5f, +0.5f,   1, 0, 0,   1, 1);
    push(+0.5f, -0.5f, +0.5f,   1, 0, 0,   1, 0);

    push(-0.5f, -0.5f, +0.5f,  -1, 0, 0,   0, 0);
    push(-0.5f, +0.5f, +0.5f,  -1, 0, 0,   0, 1);
    push(-0.5f, +0.5f, -0.5f,  -1, 0, 0,   1, 1);
    push(-0.5f, -0.5f, -0.5f,  -1, 0, 0,   1, 0);

    push(-0.5f, +0.5f, +0.5f,   0, 1, 0,   0, 0);
    push(+0.5f, +0.5f, +0.5f,   0, 1, 0,   0, 1);
    push(+0.5f, +0.5f, -0.5f,   0, 1, 0,   1, 1);
    push(-0.5f, +0.5f, -0.5f,   0, 1, 0,   1, 0);

    push(-0.5f, -0.5f, -0.5f,   0,-1, 0,   0, 0);
    push(+0.5f, -0.5f, -0.5f,   0,-1, 0,   0, 1);
    push(+0.5f, -0.5f, +0.5f,   0,-1, 0,   1, 1);
    push(-0.5f, -0.5f, +0.5f,   0,-1, 0,   1, 0);

    push(-0.5f, -0.5f, +0.5f,   0, 0, 1,   0, 0);
    push(+0.5f, -0.5f, +0.5f,   0, 0, 1,   0, 1);
    push(+0.5f, +0.5f, +0.5f,   0, 0, 1,   1, 1);
    push(-0.5f, +0.5f, +0.5f,   0, 0, 1,   1, 0);

    push(+0.5f, -0.5f, -0.5f,   0, 0,-1,   0, 0);
    push(-0.5f, -0.5f, -0.5f,   0, 0,-1,   0, 1);
    push(-0.5f, +0.5f, -0.5f,   0, 0,-1,   1, 1);
    push(+0.5f, +0.5f, -0.5f,   0, 0,-1,   1, 0);

    UINT16 y = 0;
    for (UINT i = 0; i < kCubeVerts / 4; ++i) {
        UINT16 base = static_cast<UINT16>(i * 4);
        idx[y++] = base;
        idx[y++] = static_cast<UINT16>(base + 1);
        idx[y++] = static_cast<UINT16>(base + 2);
        idx[y++] = base;
        idx[y++] = static_cast<UINT16>(base + 2);
        idx[y++] = static_cast<UINT16>(base + 3);
    }

    // floor: 4 verts already in world space; normal is +Y; never moves.
    const float half = 20.0f;
    const float fy   = -1.2f;
    UINT16 fbase = static_cast<UINT16>(kCubeVerts);
    UINT f = kCubeVerts;
    auto floorVert = [&](float fx, float fz) {
        v[f++] = {{fx, fy, fz}, {0, 1, 0}, {fx * 0.25f, fz * 0.25f}, 0u};
    };
    floorVert(-half, -half);
    floorVert(+half, -half);
    floorVert(+half, +half);
    floorVert(-half, +half);

    idx[y++] = fbase;
    idx[y++] = static_cast<UINT16>(fbase + 1);
    idx[y++] = static_cast<UINT16>(fbase + 2);
    idx[y++] = fbase;
    idx[y++] = static_cast<UINT16>(fbase + 2);
    idx[y++] = static_cast<UINT16>(fbase + 3);
}

} // namespace

const char* UpscaleModeName(UpscaleMode mode) { return modeName(mode); }

bool UpscaleModeFromString(const std::string& s, UpscaleMode& out)
{
    return parseMode(s, out);
}

// --- default render-state builders (avoids the CD3DX12 helpers) ---------
namespace {
D3D12_RASTERIZER_DESC makeRasterizer(bool cullNone)
{
    D3D12_RASTERIZER_DESC d{};
    d.FillMode              = D3D12_FILL_MODE_SOLID;
    d.CullMode              = cullNone ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    d.FrontCounterClockwise = FALSE;
    d.DepthClipEnable       = TRUE;
    d.ConservativeRaster    = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return d;
}

D3D12_BLEND_DESC makeBlend()
{
    D3D12_BLEND_DESC d{};
    d.AlphaToCoverageEnable  = FALSE;
    d.IndependentBlendEnable = FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC rt = {
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL,
    };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        d.RenderTarget[i] = rt;
    return d;
}
} // namespace

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------
bool D3D12Renderer::initialize(HWND hwnd, UINT outputW, UINT outputH,
                               xess_quality_settings_t quality,
                               int requestedGpuIndex, UpscaleMode initMode,
                               const std::string& csvPath,
                               const std::wstring& shaderDir)
{
    m_outputW    = outputW;
    m_outputH    = outputH;
    m_mode       = initMode;
    m_recordedMode = initMode;
    m_quality    = quality;
    m_csvPath    = csvPath;
    m_shaderDir  = shaderDir;

    try {
        if (!createDeviceAndSwapchain(hwnd, requestedGpuIndex))
            throw std::runtime_error("device/swapchain creation failed");
        if (!createSyncAndCommandBuffers())
            throw std::runtime_error("command resources creation failed");
        createDescriptorHeaps();
        if (!createPipelines())
            throw std::runtime_error("pipeline creation failed");
        createGeometry();
        if (!createSceneTargets())
            throw std::runtime_error("scene target creation failed");
        if (!m_gpuClock.initialize(m_device.Get()))
            throw std::runtime_error("GpuClock init failed");
        UINT64 frequency = 0;
        ThrowIfFailed(m_queue->GetTimestampFrequency(&frequency),
                      "GetTimestampFrequency");
        m_gpuClock.setFrequency(frequency);
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[renderer] init failed: %s\n", e.what());
        shutdown();
        return false;
    }

    std::printf("[renderer] output %ux%u | mode %s | scene %ux%u | xess %s\n",
                m_outputW, m_outputH, UpscaleModeName(m_mode),
                m_sceneW, m_sceneH, m_xessReady ? "ready" : "off");
    return true;
}

void D3D12Renderer::shutdown()
{
    if (m_queue && m_fence) {
        UINT64 v = ++m_nextFenceValue;
        m_queue->Signal(m_fence.Get(), v);
        waitForFence(v);
    }
    releaseSceneTargets();
    m_gpuClock.shutdown();
    m_xess.reset();

    for (auto& rt : m_renderTargets) rt.Reset();
    m_vbUpload.Reset(); m_ibUpload.Reset();
    m_vertexBuffer.Reset(); m_indexBuffer.Reset();
    for (auto& cb : m_cbUpload) cb.Reset();
    for (auto& a : m_cmdAlloc) a.Reset();
    m_cmdList.Reset();
    if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
    m_fence.Reset();
    m_rtvHeap.Reset(); m_sceneRtvHeap.Reset(); m_dsvHeap.Reset(); m_srvUavHeap.Reset();
    m_swapchain.Reset();
    m_queue.Reset();
    m_device.Reset();
    m_factory.Reset();
}

// ---------------------------------------------------------------------------
// device / swapchain
// ---------------------------------------------------------------------------
bool D3D12Renderer::createDeviceAndSwapchain(HWND hwnd, int requestedGpuIndex)
{
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> dbg;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dbg.GetAddressOf()))))
        dbg->EnableDebugLayer();
#endif

    m_adapter = intel::AdapterSelector::pick(requestedGpuIndex);
    if (!m_adapter) {
        std::fprintf(stderr, "[gpu] no suitable DXGI adapter found\n");
        return false;
    }
    DXGI_ADAPTER_DESC1 adesc{};
    m_adapter->GetDesc1(&adesc);
    std::printf("[gpu] %ls | vendor 0x%04X device 0x%04X\n",
                adesc.Description, adesc.VendorId, adesc.DeviceId);

    ThrowIfFailed(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_1,
                                    IID_PPV_ARGS(m_device.GetAddressOf())),
                  "D3D12CreateDevice");

#if defined(_DEBUG)
    // Route debug-layer messages to stderr (no debugger needed).
    ComPtr<ID3D12InfoQueue1> iq1;
    if (SUCCEEDED(m_device.As(&iq1))) {
        iq1->RegisterMessageCallback(
            &d3d12DebugCallback,
            D3D12_MESSAGE_CALLBACK_FLAG_NONE, 0, nullptr);
        std::printf("[d3d12] debug-layer message callback registered\n");
    } else {
        std::printf("[d3d12] InfoQueue1 unavailable\n");
    }
#endif

    D3D12_COMMAND_QUEUE_DESC qdesc{};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&qdesc,
                                               IID_PPV_ARGS(m_queue.GetAddressOf())),
                  "CreateCommandQueue");

#if defined(_DEBUG)
    ThrowIfFailed(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,
                                     IID_PPV_ARGS(m_factory.GetAddressOf())),
                  "CreateDXGIFactory2(debug)");
#else
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(m_factory.GetAddressOf())),
                  "CreateDXGIFactory1");
#endif

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width       = m_outputW;
    scd.Height      = m_outputH;
    scd.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc  = {1, 0};
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kFrameCount;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &scd,
                                                    nullptr, nullptr,
                                                    sc1.GetAddressOf()),
                  "CreateSwapChainForHwnd");
    ThrowIfFailed(sc1.As(&m_swapchain), "IDXGISwapChain3");
    m_backBufferCount = kFrameCount;

    return true;
}

// ---------------------------------------------------------------------------
// sync / command buffers
// ---------------------------------------------------------------------------
bool D3D12Renderer::createSyncAndCommandBuffers()
{
    ThrowIfFailed(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                        IID_PPV_ARGS(m_fence.GetAddressOf())),
                  "CreateFence");
    m_fenceEvent = CreateEventExW(nullptr, FALSE, FALSE, EVENT_MODIFY_STATE);
    if (!m_fenceEvent)
        return false;

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(m_device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(m_cmdAlloc[i].GetAddressOf())),
            "CreateCommandAllocator");
    }
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              m_cmdAlloc[0].Get(), nullptr,
                                              IID_PPV_ARGS(m_cmdList.GetAddressOf())),
                  "CreateCommandList");
    ThrowIfFailed(m_cmdList->Close(), "command list Close");
    return true;
}

// ---------------------------------------------------------------------------
// heaps
// ---------------------------------------------------------------------------
void D3D12Renderer::createDescriptorHeaps()
{
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_srvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = m_backBufferCount;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&rtvDesc,
                                                 IID_PPV_ARGS(m_rtvHeap.GetAddressOf())),
                  "rtv heap");

    D3D12_DESCRIPTOR_HEAP_DESC sceneRtvDesc = rtvDesc;
    sceneRtvDesc.NumDescriptors = 4;      // [0] color, [1] velocity, [2] fsr easu out
    ThrowIfFailed(m_device->CreateDescriptorHeap(&sceneRtvDesc,
                                                 IID_PPV_ARGS(m_sceneRtvHeap.GetAddressOf())),
                  "scene rtv heap");

    D3D12_DESCRIPTOR_HEAP_DESC dsvDesc{};
    dsvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvDesc.NumDescriptors = 1;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&dsvDesc,
                                                 IID_PPV_ARGS(m_dsvHeap.GetAddressOf())),
                  "dsv heap");

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type            = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.Flags           = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    srvDesc.NumDescriptors  = 8;   // [0] color srv, [1] xess out srv (uav is SDK-owned)
    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvDesc,
                                                 IID_PPV_ARGS(m_srvUavHeap.GetAddressOf())),
                  "srv/uav heap");

    // back buffer RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < m_backBufferCount; ++i) {
        ThrowIfFailed(m_swapchain->GetBuffer(i,
                        IID_PPV_ARGS(m_renderTargets[i].GetAddressOf())),
                      "swapchain GetBuffer");
        m_device->CreateRenderTargetView(m_renderTargets[i].Get(), nullptr, rtv);
        rtv.ptr += m_rtvSize;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::backBufferRtv(UINT frameIndex) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(frameIndex) * m_rtvSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::sceneRtvSlot(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_sceneRtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_rtvSize;
    return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::srvUavSlot(UINT slot) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvUavHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_srvSize;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::srvUavGpuSlot(UINT slot) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvUavHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(slot) * m_srvSize;
    return h;
}

// ---------------------------------------------------------------------------
// pipelines
// ---------------------------------------------------------------------------
ComPtr<ID3DBlob> D3D12Renderer::compileShader(const std::wstring& file,
                                              const char* entry,
                                              const char* target) const
{
    ComPtr<ID3DBlob> blob, err;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompileFromFile(file.c_str(), nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDES,
                                    entry, target, flags, 0,
                                    blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        if (err) {
            std::fprintf(stderr, "[shader] %ls:%s\n%.*s\n", file.c_str(), entry,
                         static_cast<int>(err->GetBufferSize()),
                         static_cast<const char*>(err->GetBufferPointer()));
        } else {
            std::fprintf(stderr, "[shader] %ls:%s failed (0x%08lX)\n",
                         file.c_str(), entry, static_cast<unsigned long>(hr));
        }
        return nullptr;
    }
    return blob;
}

ComPtr<ID3D12RootSignature> D3D12Renderer::buildRootSig(
    const D3D12_ROOT_SIGNATURE_DESC& desc) const
{
    ComPtr<ID3DBlob> signature, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                             signature.GetAddressOf(),
                                             error.GetAddressOf());
    if (FAILED(hr)) {
        if (error)
            std::fprintf(stderr, "[rootsig] %.*s\n",
                         static_cast<int>(error->GetBufferSize()),
                         static_cast<const char*>(error->GetBufferPointer()));
        return nullptr;
    }
    ComPtr<ID3D12RootSignature> result;
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(result.GetAddressOf())),
                  "CreateRootSignature");
    return result;
}

bool D3D12Renderer::createPipelines()
{
    // --- scene pass: one root CBV (b0), no static sampler -------------------
    D3D12_ROOT_PARAMETER sceneParam{};
    sceneParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    sceneParam.Descriptor.ShaderRegister = 0;
    sceneParam.Descriptor.RegisterSpace  = 0;
    sceneParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC sceneSigDesc{};
    sceneSigDesc.NumParameters       = 1;
    sceneSigDesc.pParameters         = &sceneParam;
    sceneSigDesc.NumStaticSamplers   = 0;
    sceneSigDesc.pStaticSamplers     = nullptr;
    sceneSigDesc.Flags               = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    m_scenePso.rootSig = buildRootSig(sceneSigDesc);
    if (!m_scenePso.rootSig) return false;

    // --- blit pass: SRV table (t0) + static linear sampler ------------------
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType                          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors                     = 1;
    range.BaseShaderRegister                 = 0;
    range.RegisterSpace                      = 0;
    range.OffsetInDescriptorsFromTableStart  = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER blitParam{};
    blitParam.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    blitParam.DescriptorTable  = {1, &range};
    blitParam.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC blitSampler{};
    blitSampler.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    blitSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    blitSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    blitSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    blitSampler.ShaderRegister   = 0;
    blitSampler.RegisterSpace    = 0;
    blitSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC blitSigDesc{};
    blitSigDesc.NumParameters     = 1;
    blitSigDesc.pParameters       = &blitParam;
    blitSigDesc.NumStaticSamplers = 1;
    blitSigDesc.pStaticSamplers   = &blitSampler;
    blitSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    m_blitPso.rootSig = buildRootSig(blitSigDesc);
    if (!m_blitPso.rootSig) return false;

    // --- scene PSO ----------------------------------------------------------
    auto vs = compileShader(m_shaderDir + L"\\scene.hlsl", "vsMain", "vs_5_0");
    auto ps = compileShader(m_shaderDir + L"\\scene.hlsl", "psMain", "ps_5_0");
    if (!vs || !ps) return false;

    D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,    0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32_UINT,        0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.InputLayout                    = {layout, _countof(layout)};
    psoDesc.pRootSignature                 = m_scenePso.rootSig.Get();
    psoDesc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    psoDesc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    psoDesc.RasterizerState                = makeRasterizer(true);
    psoDesc.BlendState                     = makeBlend();
    psoDesc.DepthStencilState.DepthEnable  = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc    = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.SampleMask                     = UINT_MAX;
    psoDesc.PrimitiveTopologyType          = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets               = 2;
    psoDesc.RTVFormats[0]                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[1]                  = DXGI_FORMAT_R16G16_FLOAT;
    psoDesc.DSVFormat                      = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc                     = {1, 0};

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
                      &psoDesc, IID_PPV_ARGS(m_scenePso.pso.GetAddressOf())),
                  "scene PSO");

    // --- blit PSO ------------------------------------------------------------
    auto bvs = compileShader(m_shaderDir + L"\\blit.hlsl", "vsMain", "vs_5_0");
    auto bps = compileShader(m_shaderDir + L"\\blit.hlsl", "psMain", "ps_5_0");
    if (!bvs || !bps) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC blitDesc{};
    blitDesc.InputLayout                   = {};
    blitDesc.pRootSignature                = m_blitPso.rootSig.Get();
    blitDesc.VS = {bvs->GetBufferPointer(), bvs->GetBufferSize()};
    blitDesc.PS = {bps->GetBufferPointer(), bps->GetBufferSize()};
    blitDesc.RasterizerState = makeRasterizer(true);
    blitDesc.BlendState      = makeBlend();
    blitDesc.DepthStencilState.DepthEnable = FALSE;
    blitDesc.DepthStencilState.StencilEnable = FALSE;
    blitDesc.SampleMask                    = UINT_MAX;
    blitDesc.PrimitiveTopologyType         = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    blitDesc.NumRenderTargets              = 1;
    blitDesc.RTVFormats[0]                 = DXGI_FORMAT_R8G8B8A8_UNORM;
    blitDesc.SampleDesc                    = {1, 0};

    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
                      &blitDesc, IID_PPV_ARGS(m_blitPso.pso.GetAddressOf())),
                  "blit PSO");

    // --- FSR 1 passes (EASU + RCAS, Phase 2) --------------------------------
    // Root: [0] SRV table (pixel), [1] root CBV b0 (constants), sampler s0.
    D3D12_ROOT_PARAMETER fsrParams[2]{};
    fsrParams[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fsrParams[0].DescriptorTable  = {1, &range};
    fsrParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    fsrParams[1].ParameterType             = D3D12_ROOT_PARAMETER_TYPE_CBV;
    fsrParams[1].Descriptor.ShaderRegister = 0;
    fsrParams[1].Descriptor.RegisterSpace  = 0;
    fsrParams[1].ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC fsrSampler{};
    fsrSampler.Filter           = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    fsrSampler.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fsrSampler.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fsrSampler.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    fsrSampler.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
    fsrSampler.MaxLOD           = D3D12_FLOAT32_MAX;
    fsrSampler.ShaderRegister   = 0;
    fsrSampler.RegisterSpace    = 0;
    fsrSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC fsrSigDesc{};
    fsrSigDesc.NumParameters     = 2;
    fsrSigDesc.pParameters       = fsrParams;
    fsrSigDesc.NumStaticSamplers = 1;
    fsrSigDesc.pStaticSamplers   = &fsrSampler;
    fsrSigDesc.Flags             = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    m_fsrEasuPso.rootSig = buildRootSig(fsrSigDesc);
    if (!m_fsrEasuPso.rootSig) return false;
    m_fsrRcasPso.rootSig = m_fsrEasuPso.rootSig;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC fsrDesc{};
    fsrDesc.pRootSignature                = m_fsrEasuPso.rootSig.Get();
    fsrDesc.RasterizerState = makeRasterizer(true);
    fsrDesc.BlendState      = makeBlend();
    fsrDesc.DepthStencilState.DepthEnable   = FALSE;
    fsrDesc.DepthStencilState.StencilEnable = FALSE;
    fsrDesc.SampleMask                    = UINT_MAX;
    fsrDesc.PrimitiveTopologyType         = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    fsrDesc.NumRenderTargets              = 1;
    fsrDesc.SampleDesc                    = {1, 0};

    auto easuVs = compileShader(m_shaderDir + L"\\fsr_easu.hlsl", "vsMain", "vs_5_0");
    auto easuPs = compileShader(m_shaderDir + L"\\fsr_easu.hlsl", "psMain", "ps_5_0");
    auto rcasPs = compileShader(m_shaderDir + L"\\fsr_rcas.hlsl", "psMain", "ps_5_0");
    if (!easuVs || !easuPs || !rcasPs) return false;

    fsrDesc.VS = {easuVs->GetBufferPointer(), easuVs->GetBufferSize()};
    fsrDesc.PS = {easuPs->GetBufferPointer(), easuPs->GetBufferSize()};
    fsrDesc.RTVFormats[0]  = DXGI_FORMAT_R8G8B8A8_UNORM;   // EASU → intermediate
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
                      &fsrDesc, IID_PPV_ARGS(m_fsrEasuPso.pso.GetAddressOf())),
                  "fsr easu PSO");

    auto rcasVs = compileShader(m_shaderDir + L"\\fsr_rcas.hlsl", "vsMain", "vs_5_0");
    if (!rcasVs) return false;
    fsrDesc.VS = {rcasVs->GetBufferPointer(), rcasVs->GetBufferSize()};
    fsrDesc.PS = {rcasPs->GetBufferPointer(), rcasPs->GetBufferSize()};
    fsrDesc.RTVFormats[0]  = DXGI_FORMAT_R8G8B8A8_UNORM;   // RCAS → back buffer
    ThrowIfFailed(m_device->CreateGraphicsPipelineState(
                      &fsrDesc, IID_PPV_ARGS(m_fsrRcasPso.pso.GetAddressOf())),
                  "fsr rcas PSO");
    return true;
}

// ---------------------------------------------------------------------------
// geometry upload
// ---------------------------------------------------------------------------
void D3D12Renderer::createGeometry()
{
    std::array<GeoVertex, kTotalVerts>  verts{};
    std::array<UINT16, kTotalIndices>   indices{};
    fillGeometry(verts.data(), indices.data());
    m_vertexCount = kTotalVerts;
    m_indexCount  = kTotalIndices;

    auto uploadBuffer = [&](const void* data, UINT bytes,
                            ComPtr<ID3D12Resource>& out,
                            ComPtr<ID3D12Resource>& uploadIt, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES defProp{};
        defProp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_HEAP_PROPERTIES upProp{};
        upProp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension          = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width              = bytes;
        desc.Height             = 1;
        desc.DepthOrArraySize   = 1;
        desc.MipLevels          = 1;
        desc.SampleDesc.Count   = 1;
        desc.Layout             = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailedW(m_device->CreateCommittedResource(
            &defProp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(out.GetAddressOf())), name);
        ThrowIfFailedW(m_device->CreateCommittedResource(
            &upProp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(uploadIt.GetAddressOf())), name);

        void* mapped = nullptr;
        D3D12_RANGE zero = {0, 0};
        ThrowIfFailed(uploadIt->Map(0, &zero, &mapped), "Map upload");
        std::memcpy(mapped, data, bytes);
        uploadIt->Unmap(0, nullptr);

        ThrowIfFailed(m_cmdList->Reset(m_cmdAlloc[0].Get(), nullptr), "cmd list Reset");
        m_cmdList->CopyBufferRegion(out.Get(), 0, uploadIt.Get(), 0, bytes);
        D3D12_RESOURCE_BARRIER bar{};
        bar.Type                        = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource        = out.Get();
        bar.Transition.StateBefore      = D3D12_RESOURCE_STATE_COMMON;
        bar.Transition.StateAfter       = D3D12_RESOURCE_STATE_GENERIC_READ;
        bar.Transition.Subresource      = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_cmdList->ResourceBarrier(1, &bar);
        ThrowIfFailed(m_cmdList->Close(), "cmd list Close");

        ID3D12CommandList* lists[] = {m_cmdList.Get()};
        m_queue->ExecuteCommandLists(
            _countof(lists), reinterpret_cast<ID3D12CommandList**>(lists));
        ThrowIfFailed(m_queue->Signal(m_fence.Get(), ++m_nextFenceValue), "Signal");
        waitForFence(m_nextFenceValue);
        ThrowIfFailed(m_cmdAlloc[0]->Reset(), "cmd alloc Reset");
    };

    uploadBuffer(verts.data(), sizeof(GeoVertex) * kTotalVerts,
                 m_vertexBuffer, m_vbUpload, L"vertex buffer");
    uploadBuffer(indices.data(), sizeof(UINT16) * kTotalIndices,
                 m_indexBuffer, m_ibUpload, L"index buffer");

    m_vbv.BufferLocation = m_vertexBuffer->GetGPUVirtualAddress();
    m_vbv.SizeInBytes    = sizeof(GeoVertex) * kTotalVerts;
    m_vbv.StrideInBytes  = sizeof(GeoVertex);
    m_ibv.BufferLocation = m_indexBuffer->GetGPUVirtualAddress();
    m_ibv.SizeInBytes    = sizeof(UINT16) * kTotalIndices;
    m_ibv.Format         = DXGI_FORMAT_R16_UINT;
}

// ---------------------------------------------------------------------------
// scene targets
// ---------------------------------------------------------------------------
bool D3D12Renderer::createSceneTargets()
{
    releaseSceneTargets();

    // XeSS context is created on demand (mode "xess").
    if (m_mode == UpscaleMode::XeSS && !m_xess) {
        m_xess = std::make_unique<XessUpscaler>();
        try {
            m_xess->initialize(m_device.Get(), m_outputW, m_outputH, m_quality);
            m_xessReady   = true;
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[xess] init failed: %s\n", e.what());
            std::fprintf(stderr, "[xess] falling back to bilinear control mode\n");
            m_xess.reset();
            m_xessReady = false;
            m_mode = UpscaleMode::Bilinear;
            m_recordedMode = m_mode;
        }
    }

    UINT sceneW = m_outputW, sceneH = m_outputH;
    if (m_mode == UpscaleMode::XeSS && m_xessReady) {
        m_xess->getInputResolution(m_outputW, m_outputH, sceneW, sceneH);
    } else if (m_mode == UpscaleMode::Fsr) {
        const float ratio = fsr1::scaleRatioFor(m_quality);
        sceneW = std::lround(static_cast<float>(m_outputW) / ratio);
        sceneH = std::lround(static_cast<float>(m_outputH) / ratio);
        if (sceneW < 1u) sceneW = 1u;
        if (sceneH < 1u) sceneH = 1u;
    }
    m_sceneW = sceneW;
    m_sceneH = sceneH;

    auto makeTarget = [&](UINT w, UINT h, DXGI_FORMAT fmt,
                          D3D12_RESOURCE_FLAGS flags,
                          D3D12_RESOURCE_STATES initState,
                          const D3D12_CLEAR_VALUE* clear,
                          const wchar_t* name) -> ComPtr<ID3D12Resource>
    {
        ComPtr<ID3D12Resource> res;
        D3D12_HEAP_PROPERTIES props{};
        props.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width              = w;
        desc.Height             = h;
        desc.MipLevels          = 1;
        desc.DepthOrArraySize   = 1;
        desc.Format             = fmt;
        desc.SampleDesc         = {1, 0};
        desc.Flags              = flags;
        ThrowIfFailedW(m_device->CreateCommittedResource(
            &props, D3D12_HEAP_FLAG_NONE, &desc, initState, clear,
            IID_PPV_ARGS(res.GetAddressOf())), name);
        HRESULT ddr = m_device->GetDeviceRemovedReason();
        if (ddr != S_OK)
            std::fprintf(stderr, "[gpu] device removed reason 0x%08lX\n",
                         static_cast<unsigned long>(ddr));
        res->SetName(name);
        return res;
    };

    D3D12_CLEAR_VALUE colorClear{};
    colorClear.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_sceneColor = makeTarget(sceneW, sceneH, DXGI_FORMAT_R16G16B16A16_FLOAT,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_RENDER_TARGET, &colorClear,
                              L"scene color");
    D3D12_CLEAR_VALUE velClear{};
    velClear.Format = DXGI_FORMAT_R16G16_FLOAT;
    m_sceneVelocity = makeTarget(sceneW, sceneH, DXGI_FORMAT_R16G16_FLOAT,
                                 D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &velClear,
                                 L"scene velocity");
    D3D12_CLEAR_VALUE depthClear{};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    m_sceneDepth = makeTarget(sceneW, sceneH, DXGI_FORMAT_D32_FLOAT,
                              D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
                              L"scene depth");

    // RTVs of the scene targets.
    D3D12_RENDER_TARGET_VIEW_DESC crt{};
    crt.Format          = DXGI_FORMAT_R16G16B16A16_FLOAT;
    crt.ViewDimension   = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sceneColor.Get(), &crt, sceneRtvSlot(0));

    D3D12_RENDER_TARGET_VIEW_DESC vrt{};
    vrt.Format          = DXGI_FORMAT_R16G16_FLOAT;
    vrt.ViewDimension   = D3D12_RTV_DIMENSION_TEXTURE2D;
    m_device->CreateRenderTargetView(m_sceneVelocity.Get(), &vrt, sceneRtvSlot(1));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format          = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension   = D3D12_DSV_DIMENSION_TEXTURE2D;
    m_device->CreateDepthStencilView(m_sceneDepth.Get(), &dsv,
                                     m_dsvHeap->GetCPUDescriptorHandleForHeapStart());

    // SRV for the scene color (used by the blit pass in bilinear/native).
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format                  = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels     = 1;   // single-mip: explicit > zero ("all")
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    m_device->CreateShaderResourceView(m_sceneColor.Get(), &srv, srvUavSlot(0));

    // XeSS output texture + views (only when XeSS actually runs).
    if (m_mode == UpscaleMode::XeSS && m_xessReady) {
        if (!m_xessOutput) {
            m_xessOutput = makeTarget(m_outputW, m_outputH,
                                      DXGI_FORMAT_R16G16B16A16_FLOAT,
                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                      L"xess output");
            m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        m_device->CreateShaderResourceView(m_xessOutput.Get(), &srv, srvUavSlot(1));
    }

    // FSR 1 resources: EASU intermediate (RGBA8 @ output res) + constants.
    if (m_mode == UpscaleMode::Fsr) {
        // Sanity: an existing intermediate must match the current output size
        // (the PoC's output size is fixed per process, so this is always true;
        // if it ever changed we release so a matching one is created next time).
        if (m_fsrEasuOut) {
            D3D12_RESOURCE_DESC d = m_fsrEasuOut->GetDesc();
            if (d.Width != m_outputW || d.Height != m_outputH ||
                d.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
                m_fsrEasuOut.Reset();
                m_fsrEasuState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
        }
        if (!m_fsrEasuOut) {
            m_fsrEasuOut = makeTarget(m_outputW, m_outputH,
                                      DXGI_FORMAT_R8G8B8A8_UNORM,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                      D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
                                      L"fsr easu output");
            m_fsrEasuState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        D3D12_RENDER_TARGET_VIEW_DESC frt{};
        frt.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
        frt.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        m_device->CreateRenderTargetView(m_fsrEasuOut.Get(), &frt, sceneRtvSlot(2));

        D3D12_SHADER_RESOURCE_VIEW_DESC fsrv{};
        fsrv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        fsrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        fsrv.Texture2D.MipLevels     = 1;
        fsrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        m_device->CreateShaderResourceView(m_fsrEasuOut.Get(), &fsrv, srvUavSlot(2));

        if (!m_fsrConstCb) {
            D3D12_HEAP_PROPERTIES upProp{};
            upProp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC cbDesc{};
            cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
            cbDesc.Width            = 256;          // 80 B payload, 256-aligned
            cbDesc.Height           = 1;
            cbDesc.DepthOrArraySize = 1;
            cbDesc.MipLevels        = 1;
            cbDesc.SampleDesc.Count = 1;
            cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ThrowIfFailedW(m_device->CreateCommittedResource(
                &upProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(m_fsrConstCb.GetAddressOf())), L"fsr constants");
        }
        // Constants are static per (sceneW, sceneH, outputW, outputH):
        // rewrite whenever the scene resolution can change.
        void* mapped = nullptr;
        D3D12_RANGE zero = {0, 0};
        ThrowIfFailed(m_fsrConstCb->Map(0, &zero, &mapped), "fsr constants Map");
        fsr1::computeConstants(m_sceneW, m_sceneH, m_outputW, m_outputH,
                               m_fsrSharpness,
                               static_cast<fsr1::Constants*>(mapped));
        D3D12_RANGE written = {0, sizeof(fsr1::Constants)};
        m_fsrConstCb->Unmap(0, &written);
    }

    // Per-frame constant upload buffers.
    const UINT cbSize = (sizeof(FrameConstants) + 255) & ~255u;
    for (UINT i = 0; i < kFrameCount; ++i) {
        D3D12_HEAP_PROPERTIES upProp{};
        upProp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC cbDesc{};
        cbDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        cbDesc.Width            = cbSize;
        cbDesc.Height           = 1;
        cbDesc.DepthOrArraySize = 1;
        cbDesc.MipLevels        = 1;
        cbDesc.SampleDesc.Count = 1;
        cbDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ThrowIfFailed(m_device->CreateCommittedResource(
            &upProp, D3D12_HEAP_FLAG_NONE, &cbDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(m_cbUpload[i].GetAddressOf())),
            "constant buffer");

        void* mapped = nullptr;
        D3D12_RANGE zero = {0, 0};
        // Map on an upload-heap buffer with no read range; returns S_OK.
        m_cbUpload[i]->Map(0, &zero, &mapped);
        m_cbMapped[i] = mapped;
        std::memset(mapped, 0, cbSize);
    }

    m_colorState    = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_velocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_depthState    = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    m_recordedMode = m_mode;
    return true;
}

void D3D12Renderer::releaseSceneTargets()
{
    m_sceneColor.Reset();
    m_sceneVelocity.Reset();
    m_sceneDepth.Reset();
    // XeSS output persists across mode swaps.
    for (auto& cb : m_cbUpload) cb.Reset();
}

// ---------------------------------------------------------------------------
// animation & frame constants
// ---------------------------------------------------------------------------
void D3D12Renderer::updateAnimation(double dtSeconds)
{
    if (!paused) {
        m_cameraTheta += static_cast<float>(dtSeconds * 0.12);
        m_cubeAngle   += static_cast<float>(dtSeconds * 0.65);
    }
    m_jitterX = 0.0f;
    m_jitterY = 0.0f;
    if (m_mode == UpscaleMode::XeSS && m_xessReady) {
        const UINT seqLen = m_xess->getJitterSequenceLength();
        UINT seqIndex = static_cast<UINT>(m_frameCounter % seqLen) + 1;
        m_jitterX = haltonSample(seqIndex, 2) - 0.5f;
        m_jitterY = haltonSample(seqIndex, 3) - 0.5f;
    }
}

void D3D12Renderer::computeFrameConstants(UINT frameIndex)
{
    FrameConstants consts{};

    // camera orbit
    float theta = m_cameraTheta;
    XMVECTOR camPos = XMVectorSet(4.2f * std::cos(theta), 2.0f,
                                  4.2f * std::sin(theta), 1.0f);
    XMVECTOR lookAt = XMVectorSet(0.0f, -0.4f, 0.0f, 1.0f);
    XMVECTOR upV    = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    XMMATRIX view = XMMatrixLookAtLH(camPos, lookAt, upV);
    float aspect = static_cast<float>(m_sceneW) / static_cast<float>(m_sceneH);
    XMMATRIX proj = XMMatrixPerspectiveFovLH(70.0f * XM_PI / 180.0f, aspect, 0.1f, 100.0f);
    XMMATRIX viewProj = view * proj;
    XMStoreFloat4x4(&consts.currViewProj, XMMatrixTranspose(viewProj));

    if (m_frameCounter == 0)
        m_prevViewProj = consts.currViewProj;
    consts.prevViewProj = m_prevViewProj;

    XMMATRIX world = XMMatrixRotationY(m_cubeAngle) *
                     XMMatrixTranslation(0.0f, -0.45f, 0.0f);
    XMStoreFloat4x4(&consts.currWorld, XMMatrixTranspose(world));
    if (m_frameCounter == 0)
        m_prevWorld = consts.currWorld;
    consts.prevWorld = m_prevWorld;

    consts.jitter     = {m_jitterX, m_jitterY, 0.0f, 0.0f};
    consts.resolution = {static_cast<float>(m_sceneW),
                         static_cast<float>(m_sceneH), 0.0f, 0.0f};

    // Cache this frame's matrices as the NEXT frame's "previous" reference.
    m_prevViewProj = consts.currViewProj;
    m_prevWorld    = consts.currWorld;

    std::memcpy(m_cbMapped[frameIndex], &consts, sizeof(FrameConstants));
}

void D3D12Renderer::waitForFence(UINT64 fenceValue)
{
    if (m_fence->GetCompletedValue() < fenceValue) {
        ThrowIfFailed(m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent),
                      "SetEventOnCompletion");
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

// ---------------------------------------------------------------------------
// frame recording
// ---------------------------------------------------------------------------
bool D3D12Renderer::recordCommandList(UINT frameIndex)
{
    ComPtr<ID3D12GraphicsCommandList>& cmdList = m_cmdList;

    ThrowIfFailed(m_cmdAlloc[frameIndex]->Reset(), "allocator Reset");
    ThrowIfFailed(cmdList->Reset(m_cmdAlloc[frameIndex].Get(), nullptr),
                  "command list Reset");

    ID3D12Resource* backBuffer = m_renderTargets[frameIndex].Get();

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type    = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                          D3D12_RESOURCE_STATES to) {
        bar.Transition.pResource   = res;
        bar.Transition.StateBefore = from;
        bar.Transition.StateAfter  = to;
        cmdList->ResourceBarrier(1, &bar);
    };

    // backbuffer: present -> render target
    transition(backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);

    // scene targets to writable states (skip redundant transitions)
    if (m_colorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(m_sceneColor.Get(), m_colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (m_velocityState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(m_sceneVelocity.Get(), m_velocityState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_velocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (m_depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        transition(m_sceneDepth.Get(), m_depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    m_gpuClock.recordSceneStart(cmdList.Get());

    // clear
    const float clearColor[4] = {0.06f, 0.09f, 0.12f, 1.0f};
    const float zero4[4]      = {0.0f, 0.0f, 0.0f, 0.0f};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
        m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    cmdList->ClearRenderTargetView(sceneRtvSlot(0), clearColor, 0, nullptr);
    cmdList->ClearRenderTargetView(sceneRtvSlot(1), zero4, 0, nullptr);
    cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // draw scene at scene resolution
    D3D12_VIEWPORT vp{0.0f, 0.0f, float(m_sceneW), float(m_sceneH), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, LONG(m_sceneW), LONG(m_sceneH)};
    cmdList->RSSetViewports(1, &vp);
    cmdList->RSSetScissorRects(1, &sc);

    ID3D12DescriptorHeap* heaps[] = {m_srvUavHeap.Get()};

    cmdList->SetGraphicsRootSignature(m_scenePso.rootSig.Get());
    cmdList->SetPipelineState(m_scenePso.pso.Get());
    cmdList->SetGraphicsRootConstantBufferView(
        0, m_cbUpload[frameIndex]->GetGPUVirtualAddress());
    cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmdList->IASetVertexBuffers(0, 1, &m_vbv);
    cmdList->IASetIndexBuffer(&m_ibv);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {sceneRtvSlot(0), sceneRtvSlot(1)};
    cmdList->OMSetRenderTargets(2, rtvs, FALSE, &dsvHandle);
    cmdList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);

    m_gpuClock.recordSceneEnd(cmdList.Get());

    bool useXess = (m_mode == UpscaleMode::XeSS && m_xessReady);

    // FSR 1 only when all of its resources exist with matching dimensions;
    // otherwise silently fall back to the bilinear blit (never crash mid-game).
    bool useFsr = false;
    if (m_mode == UpscaleMode::Fsr && m_fsrConstCb && m_fsrEasuOut) {
        D3D12_RESOURCE_DESC d = m_fsrEasuOut->GetDesc();
        useFsr = (d.Width == m_outputW && d.Height == m_outputH);
    }

    if (useXess) {
        // scene inputs -> NON_PIXEL_SHADER_RESOURCE (SDK requirement)
        transition(m_sceneColor.Get(), m_colorState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_colorState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition(m_sceneVelocity.Get(), m_velocityState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_velocityState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition(m_sceneDepth.Get(), m_depthState,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        // XeSS-SR execute (per Intel sample: jitterOffsetY = -jitter[1])
        if (!m_xess->upscale(cmdList.Get(), m_sceneColor.Get(),
                             m_sceneVelocity.Get(), m_sceneDepth.Get(),
                             m_xessOutput.Get(),
                             m_jitterX, -m_jitterY,
                             m_sceneW, m_sceneH)) {
            std::fprintf(stderr, "[xess] execute failed\n");
            return false;
        }

        // xess output: Uacc -> pixel-shader readable for our blit
        transition(m_xessOutput.Get(), m_outputState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_outputState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    } else {
        // scene color -> pixel shader readable
        transition(m_sceneColor.Get(), m_colorState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_colorState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    // upscale passes run at output resolution
    D3D12_VIEWPORT outVp{0.0f, 0.0f, float(m_outputW), float(m_outputH), 0.0f, 1.0f};
    D3D12_RECT outSc{0, 0, LONG(m_outputW), LONG(m_outputH)};
    cmdList->RSSetViewports(1, &outVp);
    cmdList->RSSetScissorRects(1, &outSc);

    if (useFsr) {
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(m_fsrEasuPso.rootSig.Get());

        // EASU: scene color (t0 slot 0) -> RGBA8 intermediate
        // (the intermediate may persist in another state after a mode swap)
        if (m_fsrEasuState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            transition(m_fsrEasuOut.Get(), m_fsrEasuState,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_fsrEasuState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        cmdList->SetPipelineState(m_fsrEasuPso.pso.Get());
        cmdList->SetGraphicsRootDescriptorTable(0, srvUavGpuSlot(0));
        cmdList->SetGraphicsRootConstantBufferView(
            1, m_fsrConstCb->GetGPUVirtualAddress());
        D3D12_CPU_DESCRIPTOR_HANDLE easuRtv = sceneRtvSlot(2);
        cmdList->OMSetRenderTargets(1, &easuRtv, FALSE, nullptr);
        cmdList->DrawInstanced(3, 1, 0, 0);

        // RCAS: intermediate -> back buffer (EASU out stays pixel-readable)
        transition(m_fsrEasuOut.Get(), m_fsrEasuState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_fsrEasuState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmdList->SetPipelineState(m_fsrRcasPso.pso.Get());
        cmdList->SetGraphicsRootDescriptorTable(0, srvUavGpuSlot(2));
        D3D12_CPU_DESCRIPTOR_HANDLE backRtvFsr = backBufferRtv(frameIndex);
        cmdList->OMSetRenderTargets(1, &backRtvFsr, FALSE, nullptr);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    m_gpuClock.recordUpscaleEnd(cmdList.Get());

    // fullscreen blit into the back buffer (skipped when FSR already wrote it)
    if (!useFsr) {
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(m_blitPso.rootSig.Get());
        cmdList->SetPipelineState(m_blitPso.pso.Get());
        cmdList->SetGraphicsRootDescriptorTable(
            0, srvUavGpuSlot(useXess ? 1 : 0));
        D3D12_CPU_DESCRIPTOR_HANDLE backRtv = backBufferRtv(frameIndex);
        cmdList->OMSetRenderTargets(1, &backRtv, FALSE, nullptr);
        cmdList->DrawInstanced(3, 1, 0, 0);
    }

    // restore states for next frame
    if (useXess) {
        if (m_outputState != D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
            transition(m_xessOutput.Get(), m_outputState,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_outputState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
    }
    if (useFsr) {
        if (m_fsrEasuState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            transition(m_fsrEasuOut.Get(), m_fsrEasuState,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_fsrEasuState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }
    if (m_colorState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(m_sceneColor.Get(), m_colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (m_velocityState != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        transition(m_sceneVelocity.Get(), m_velocityState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_velocityState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (m_depthState != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        transition(m_sceneDepth.Get(), m_depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    transition(backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);

    m_gpuClock.resolve(cmdList.Get());

    ThrowIfFailed(cmdList->Close(), "command list Close");

    ID3D12CommandList* lists[] = {cmdList.Get()};
    m_queue->ExecuteCommandLists(
        _countof(lists),
        reinterpret_cast<ID3D12CommandList**>(lists));

    UINT64 fenceVal = ++m_nextFenceValue;
    ThrowIfFailed(m_queue->Signal(m_fence.Get(), fenceVal), "fence Signal");
    m_fenceValues[frameIndex] = fenceVal;

    try {
        ThrowIfFailed(m_swapchain->Present(1, 0), "Present");
    }
    catch (const std::exception&) {
        std::fprintf(stderr, "[renderer] device removed reason: 0x%08lX\n",
                     static_cast<unsigned long>(m_device->GetDeviceRemovedReason()));
        throw;
    }
    return true;
}

// ---------------------------------------------------------------------------
// frame loop
// ---------------------------------------------------------------------------
void D3D12Renderer::renderFrame()
{
    auto now = std::chrono::steady_clock::now();

    m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();
    waitForFence(m_fenceValues[m_frameIndex]);

    double dt = 1.0 / 60.0;      // first frame assumption
    if (m_frameCounter > 0)
        dt = std::chrono::duration<double>(now - m_lastStart).count();
    m_lastStart = now;

    if (m_mode != m_recordedMode) {
        // Full GPU flush before destroying resources still in use.
        UINT64 flushVal = ++m_nextFenceValue;
        ThrowIfFailed(m_queue->Signal(m_fence.Get(), flushVal), "Signal");
        waitForFence(flushVal);
        releaseSceneTargets();
        createSceneTargets();
    }

    updateAnimation(dt);
    computeFrameConstants(m_frameIndex);

    {
        char vbuf[8] = {0};
        DWORD vn = GetEnvironmentVariableA("MINOVA_VERBOSE", vbuf, sizeof(vbuf));
        if (vn > 0 && vbuf[0] == '1')
            std::fprintf(stderr, "[frame] %u mode=%s\n", m_frameCounter,
                         UpscaleModeName(m_mode));
    }

    m_gpuClock.beginFrame(m_csvPath, UpscaleModeName(m_mode), m_cpuFps);

    if (!recordCommandList(m_frameIndex))
        return;

    m_frameCounter++;

    double fps = 1000.0 / (dt * 1000.0);
    if (m_frameCounter <= 1)
        m_cpuFps = fps;
    else
        m_cpuFps = fps * 0.05 + m_cpuFps * 0.95;
}
