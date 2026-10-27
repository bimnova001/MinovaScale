#include "xess_upscaler.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <wrl/client.h>

namespace {

const char* xessResultName(xess_result_t result)
{
    switch (result) {
    case XESS_RESULT_SUCCESS:                     return "XESS_RESULT_SUCCESS";
    case XESS_RESULT_ERROR_UNSUPPORTED_DEVICE:    return "XESS_RESULT_ERROR_UNSUPPORTED_DEVICE";
    case XESS_RESULT_ERROR_UNSUPPORTED_DRIVER:    return "XESS_RESULT_ERROR_UNSUPPORTED_DRIVER";
    case XESS_RESULT_WARNING_OLD_DRIVER:          return "XESS_RESULT_WARNING_OLD_DRIVER";
    default:                                      return "XESS_RESULT_ERROR";
    }
}

void xessLogCallback(const char* message, xess_logging_level_t level)
{
    const char* tag = level >= XESS_LOGGING_LEVEL_ERROR ? "ERROR"
                    : level == XESS_LOGGING_LEVEL_WARNING ? "WARN"
                    : "info";
    std::fprintf(stderr, "[xess][%s] %s\n", tag, message ? message : "");
}

xess_result_t checkXess(xess_result_t result, const char* what)
{
    if (result != XESS_RESULT_SUCCESS) {
        char msg[512];
        std::snprintf(msg, sizeof(msg), "XeSS error 0x%08X (%s): %s",
                      static_cast<unsigned>(result), xessResultName(result), what);
        if (result == XESS_RESULT_ERROR_UNSUPPORTED_DRIVER)
            std::fprintf(stderr,
                "[xess] UNSUPPORTED_DRIVER: libxess requires a newer Intel graphics "
                "driver. XeSS 1.0+ on Intel hardware requires driver 31.0.101.4148 "
                "or later (XeSS 2.x may need an even newer one). Update via Intel Arc "
                "Control, Windows Update or intel.com - the current driver predates "
                "XeSS support.\n");
        throw std::runtime_error(msg);
    }
    return result;
}

} // namespace

xess_quality_settings_t parseXessQuality(const char* name)
{
    std::string s(name);
    if (s == "ultra_performance")  return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
    if (s == "performance")        return XESS_QUALITY_SETTING_PERFORMANCE;
    if (s == "quality")            return XESS_QUALITY_SETTING_QUALITY;
    if (s == "ultra_quality")      return XESS_QUALITY_SETTING_ULTRA_QUALITY;
    if (s == "ultra_quality_plus") return XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS;
    if (s == "aa")                 return XESS_QUALITY_SETTING_AA;
    return XESS_QUALITY_SETTING_BALANCED; // default ("balanced")
}

XessUpscaler::~XessUpscaler() { shutdown(); }

const char* XessUpscaler::getVersionString()
{
    static char s_version[32] = {0};
    if (s_version[0] == '\0') {
        xess_version_t v{};
        if (xessGetVersion(&v) == XESS_RESULT_SUCCESS)
            std::snprintf(s_version, sizeof(s_version), "%u.%u.%u",
                          v.major, v.minor, v.patch);
        else
            std::strcpy(s_version, "unavailable");
    }
    return s_version;
}

void XessUpscaler::initialize(ID3D12Device* device,
                              uint32_t outputW, uint32_t outputH,
                              xess_quality_settings_t quality)
{
    m_quality    = quality;
    m_outputRes  = {outputW, outputH};

    checkXess(xessD3D12CreateContext(device, &m_context), "xessD3D12CreateContext");

    // Route libxess internal logs to stderr (pipeline-build / validation errors).
    xessSetLoggingCallback(m_context, XESS_LOGGING_LEVEL_DEBUG,
                           &xessLogCallback);

    // Driver recommendation (per SDK docs; also prints negotiated status).
    xess_result_t driverStatus = xessIsOptimalDriver(m_context);
    if (driverStatus == XESS_RESULT_WARNING_OLD_DRIVER)
        std::fprintf(stderr,
            "[xess] notice: outdated driver detected; install the latest GPU "
            "driver for optimal XeSS performance/quality.\n");

    // NDC motion vectors: the renderer computes velocities in NDC so the
    // encoding does not depend on render resolution.
    uint32_t initFlags = XESS_INIT_FLAG_USE_NDC_VELOCITY;

    // Blocking: finish kernel compilation + pipeline creation HERE, before any
    // game frame. Without it the first executes JIT-compile everything at once
    // (~1 s/frame for the first frames) and can trip a GPU TDR
    // (DXGI_ERROR_GPU_HUNG) on slow iGPUs.
    checkXess(xessD3D12BuildPipelines(m_context, nullptr, true, initFlags),
              "xessD3D12BuildPipelines");

    // Match the official sample: pre-allocate the texture storage heap from
    // xessGetProperties and hand it to Init (avoids internal allocations).
    xess_properties_t props{};
    checkXess(xessGetProperties(m_context, &m_outputRes, &props),
              "xessGetProperties");

    Microsoft::WRL::ComPtr<ID3D12Heap> textureHeap;
    if (props.tempTextureHeapSize > 0) {
        D3D12_HEAP_DESC hd{};
        hd.SizeInBytes      = props.tempTextureHeapSize;
        hd.Properties.Type  = D3D12_HEAP_TYPE_DEFAULT;
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        if (FAILED(device->CreateHeap(&hd, IID_PPV_ARGS(textureHeap.GetAddressOf()))))
            std::fprintf(stderr, "[xess] using internal texture storage instead\n");
    }

    xess_d3d12_init_params_t params{};
    params.outputResolution = m_outputRes;
    params.qualitySetting   = m_quality;
    params.initFlags        = static_cast<xess_init_flags_t>(initFlags);
    params.pTempTextureHeap = textureHeap.Get();

    checkXess(xessD3D12Init(m_context, &params), "xessD3D12Init");
    std::printf("[xess] pipelines built\n");

    checkXess(xessGetInputResolution(m_context, &m_outputRes, m_quality, &m_inputRes),
              "xessGetInputResolution");

    std::printf("[xess] initialized: output %ux%u, input %ux%u (quality %d)\n",
                m_outputRes.x, m_outputRes.y,
                m_inputRes.x, m_inputRes.y,
                static_cast<int>(m_quality));
}

void XessUpscaler::shutdown()
{
    if (m_context) {
        xessDestroyContext(m_context);
        m_context = nullptr;
    }
}

bool XessUpscaler::getInputResolution(uint32_t outputW, uint32_t outputH,
                                      uint32_t& inputW, uint32_t& inputH)
{
    if (!m_context)
        return false;
    m_outputRes = {outputW, outputH};
    checkXess(xessGetInputResolution(m_context, &m_outputRes, m_quality, &m_inputRes),
              "xessGetInputResolution");
    inputW = m_inputRes.x;
    inputH = m_inputRes.y;
    return true;
}

bool XessUpscaler::upscale(ID3D12GraphicsCommandList* cmdList,
                           ID3D12Resource* color, ID3D12Resource* velocity,
                           ID3D12Resource* depth, ID3D12Resource* output,
                           float jitterX, float jitterY,
                           uint32_t inputW, uint32_t inputH)
{
    xess_d3d12_execute_params_t params{};
    params.inputWidth     = inputW;
    params.inputHeight    = inputH;
    params.jitterOffsetX  = jitterX;
    params.jitterOffsetY  = jitterY;
    params.exposureScale  = 1.0f;   // LDR flag not set; scene-referred linear
    params.pColorTexture    = color;
    params.pVelocityTexture = velocity;
    params.pDepthTexture    = depth;   // required with low-res MVs
    params.pOutputTexture   = output;

    return xessD3D12Execute(m_context, cmdList, &params) == XESS_RESULT_SUCCESS;
}

void XessUpscaler::logStatus() const
{
    std::printf("[xess] libxess.dll version %s\n", getVersionString());
    if (m_context) {
        xess_version_t xefx{};
        if (xessGetIntelXeFXVersion(m_context, &xefx) == XESS_RESULT_SUCCESS)
            std::printf("[xess] Intel XeFX library version %u.%u.%u\n",
                        xefx.major, xefx.minor, xefx.patch);
    }
}
