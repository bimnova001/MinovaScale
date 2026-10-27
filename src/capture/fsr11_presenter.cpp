#include "fsr11_presenter.h"

#include <windows.h>

#include "intel/adapter_selector.h"
#include "upscaling/fsr1_upscaler.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#ifndef D3D_COMPILE_STANDARD_FILE_INCLUDES
#define D3D_COMPILE_STANDARD_FILE_INCLUDES ((ID3DInclude*)(UINT_PTR)1)
#endif

using Microsoft::WRL::ComPtr;

namespace capture {

namespace {

constexpr unsigned kBackBuffers = 3;

void throwHr(HRESULT hr, const char* what)
{
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s failed (hr=0x%08lX)",
                  what, static_cast<unsigned long>(hr));
    throw std::runtime_error(msg);
}

} // namespace

ComPtr<ID3DBlob> Fsr11Presenter::compileShader(const std::wstring& file,
                                               const char* entry,
                                               const char* target) const
{
    ComPtr<ID3DBlob> blob, err;
    D3D_SHADER_MACRO defines[] = {{"MINOVA_FSR_NO_GAMMA", "1"}, {nullptr, nullptr}};
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompileFromFile(file.c_str(), defines,
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
        throw std::runtime_error("FSR shader compilation failed");
    }
    return blob;
}

void Fsr11Presenter::initialize(HWND presenterHwnd, unsigned outW, unsigned outH,
                                float sharpnessStops, int requestedGpuIndex,
                                const std::wstring& shaderDir,
                                bool uncappedPresent,
                                ComPtr<ID3D11Device>* deviceOut)
{
    try {
        m_outW = outW;
        m_outH = outH;
        m_sharpness = sharpnessStops;
        m_uncapped = uncappedPresent;

        m_adapter = intel::AdapterSelector::pick(requestedGpuIndex);
        HRESULT hr = D3D11CreateDevice(
            m_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_FLAG(D3D11_CREATE_DEVICE_BGRA_SUPPORT),
            nullptr, 0, D3D11_SDK_VERSION,
            m_device.GetAddressOf(), nullptr, m_ctx.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "D3D11CreateDevice (capture)");

        *deviceOut = m_device;

        // --- swapchain (flip discard, BGRA) ----------------------------------
        ComPtr<IDXGIFactory2> factory;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) throwHr(hr, "DXGI factory (capture)");

        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width       = outW;
        scd.Height      = outH;
        scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.BufferCount = kBackBuffers;
        scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        scd.SampleDesc  = {1, 0};
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), presenterHwnd,
                                             &scd, nullptr, nullptr,
                                             m_swapchain.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "swapchain (capture)");

        // NOTE: D3D11 flip swapchains only expose buffer 0 until buffers
        // rotate; the active back buffer is fetched fresh in render().

        // --- intermediate ----------------------------------------------------
        D3D11_TEXTURE2D_DESC itex{};
        itex.Width      = outW;
        itex.Height     = outH;
        itex.MipLevels  = 1;
        itex.ArraySize  = 1;
        itex.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
        itex.SampleDesc = {1, 0};
        itex.Usage      = D3D11_USAGE_DEFAULT;
        itex.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&itex, nullptr, m_intermediate.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "fsr intermediate (capture)");
        m_device->CreateRenderTargetView(m_intermediate.Get(), nullptr,
                                         m_midRtv.GetAddressOf());
        m_device->CreateShaderResourceView(m_intermediate.Get(), nullptr,
                                           m_midSrv.GetAddressOf());

        // --- constants buffer (EASU 64 B + RCAS 16 B payload) -----------------
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth       = 256;     // 80 B payload, 16-B padded
        cbd.Usage           = D3D11_USAGE_DYNAMIC;
        cbd.CPUAccessFlags  = D3D11_CPU_ACCESS_WRITE;
        cbd.BindFlags       = D3D11_BIND_CONSTANT_BUFFER;
        hr = m_device->CreateBuffer(&cbd, nullptr, m_cb.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "fsr cbuffer (capture)");

        // --- sampler (EASU gather taps need a linear filter) ------------------
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        hr = m_device->CreateSamplerState(&sd, m_sampler.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "fsr sampler (capture)");

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        hr = m_device->CreateRasterizerState(&rd, m_raster.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "fsr rasterizer (capture)");

        // --- shaders -----------------------------------------------------------
        auto easuVs = compileShader(shaderDir + L"\\fsr_easu.hlsl", "vsMain", "vs_5_0");
        auto easuPs = compileShader(shaderDir + L"\\fsr_easu.hlsl", "psMain", "ps_5_0");
        auto rcasVs = compileShader(shaderDir + L"\\fsr_rcas.hlsl", "vsMain", "vs_5_0");
        auto rcasPs = compileShader(shaderDir + L"\\fsr_rcas.hlsl", "psMain", "ps_5_0");

        hr = m_device->CreateVertexShader(easuVs->GetBufferPointer(),
                                          easuVs->GetBufferSize(), nullptr,
                                          m_easuVs.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "easu VS (capture)");
        hr = m_device->CreatePixelShader(easuPs->GetBufferPointer(),
                                         easuPs->GetBufferSize(), nullptr,
                                         m_easuPs.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "easu PS (capture)");
        hr = m_device->CreateVertexShader(rcasVs->GetBufferPointer(),
                                          rcasVs->GetBufferSize(), nullptr,
                                          m_rcasVs.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "rcas VS (capture)");
        hr = m_device->CreatePixelShader(rcasPs->GetBufferPointer(),
                                         rcasPs->GetBufferSize(), nullptr,
                                         m_rcasPs.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "rcas PS (capture)");

        // --- debug quadrant shader (enabled with MINOVA_DEBUG_SHADER) --------
        // Q1: nearest Load at crop offset      Q2: linear Sample at crop uv
        // Q3: nearest Load over the WHOLE src  Q4: solid red/green raster test
        if (std::getenv("MINOVA_DEBUG_SHADER")) {
            static const char* dbgHlsl = R"HLSL(
Texture2D In   : register(t0);
SamplerState s : register(s0);
cbuffer Dbg : register(b1) { float4 Crop; float4 Src; }
struct DOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 psMain(DOut i) : SV_Target {
    if (i.uv.y < 0.5) {
        if (i.uv.x < 0.5) {
            int x = int(Crop.x + i.uv.x * 2.0 * Crop.z);
            int y = int(Crop.y + i.uv.y * 2.0 * Crop.w);
            return In.Load(int3(x, y, 0));
        } else {
            float2 u2 = float2(Crop.x + (i.uv.x - 0.5) * 2.0 * Crop.z,
                               Crop.y + i.uv.y * 2.0 * Crop.w) / Src.xy;
            return In.Sample(s, u2);
        }
    }
    if (i.uv.x < 0.5) {
        int x = int(i.uv.x * 2.0 * Src.x);
        int y = int((i.uv.y - 0.5) * 2.0 * Src.y);
        return In.Load(int3(x, y, 0));
    }
    return (i.uv.y < 0.75) ? float4(1.0, 0.3, 0.3, 1.0)
                           : float4(0.3, 1.0, 0.3, 1.0);
}
)HLSL";
            ComPtr<ID3DBlob> blob, err;
            hr = D3DCompile(dbgHlsl, std::strlen(dbgHlsl), "dbg", nullptr,
                            nullptr, "psMain", "ps_5_0",
                            D3DCOMPILE_ENABLE_STRICTNESS, 0,
                            blob.GetAddressOf(), err.GetAddressOf());
            if (FAILED(hr)) {
                if (err)
                    std::fprintf(stderr, "[dbg-shader] %.*s\n",
                                 int(err->GetBufferSize()),
                                 (const char*)err->GetBufferPointer());
                throwHr(hr, "debug shader (capture)");
            }
            hr = m_device->CreatePixelShader(blob->GetBufferPointer(),
                                             blob->GetBufferSize(), nullptr,
                                             m_dbgPs.GetAddressOf());
            if (FAILED(hr)) throwHr(hr, "debug PS (capture)");

            D3D11_BUFFER_DESC dcbd{};
            dcbd.ByteWidth      = 32;    // Crop(16) + Src(16)
            dcbd.Usage          = D3D11_USAGE_DYNAMIC;
            dcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dcbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            hr = m_device->CreateBuffer(&dcbd, nullptr, m_dbgCb.GetAddressOf());
            if (FAILED(hr)) throwHr(hr, "debug cbuffer (capture)");
            std::printf("[capture] DEBUG quadrant shader enabled "
                        "(MINOVA_DEBUG_SHADER)\n");
        }

        std::printf("[capture] FSR11 presenter ready: %ux%u | RCAS %.2f stops%s\n",
                    outW, outH, m_sharpness,
                    m_uncapped ? " | uncapped (Present 0)" : "");
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "[capture] presenter init failed: %s\n", e.what());
        shutdown();
        throw;
    }
}

void Fsr11Presenter::render(ID3D11Texture2D* srcTex, unsigned srcW, unsigned srcH,
                            unsigned cropX, unsigned cropY,
                            unsigned cropW, unsigned cropH)
{
    if (!m_swapchain || !srcTex) return;
    ID3D11DeviceContext* c = m_ctx.Get();

    // --- constants ------------------------------------------------------------
    fsr1::Constants consts{};
    fsr1::computeConstantsCrop(cropW, cropH,      // input viewport (client crop)
                               srcW, srcH,        // full capture texture
                               m_outW, m_outH,
                               cropX, cropY,
                               m_sharpness, &consts);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(c->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map)))
        return;
    std::memcpy(map.pData, &consts, sizeof(consts));
    c->Unmap(m_cb.Get(), 0);

    // --- source SRV (cached; capture textures are stable) --------------------
    if (m_cachedTex.Get() != srcTex) {
        m_cachedSrv.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format              = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(srcTex, &sd, m_cachedSrv.GetAddressOf());
        m_cachedTex = srcTex;
    }
    if (!m_cachedSrv) return;

    ID3D11ShaderResourceView* nullSrv = nullptr;
    ID3D11Buffer* nullCb = nullptr;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(m_swapchain->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()))))
        return;
    ComPtr<ID3D11RenderTargetView> backRtv;
    m_device->CreateRenderTargetView(back.Get(), nullptr,
                                     backRtv.GetAddressOf());

    // --- debug quadrant pass replaces the whole pipeline when enabled --------
    if (m_dbgPs) {
        D3D11_MAPPED_SUBRESOURCE dmap{};
        if (SUCCEEDED(c->Map(m_dbgCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dmap))) {
            float d[8] = {float(cropX), float(cropY), float(cropW), float(cropH),
                          float(srcW),  float(srcH),  0.0f,           0.0f};
            std::memcpy(dmap.pData, d, sizeof(d));
            c->Unmap(m_dbgCb.Get(), 0);
        }
        ID3D11Buffer* dcb = m_dbgCb.Get();
        c->OMSetRenderTargets(1, backRtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT dvp{0.0f, 0.0f, float(m_outW), float(m_outH), 0.0f, 1.0f};
        c->RSSetViewports(1, &dvp);
        c->RSSetState(m_raster.Get());
        c->PSSetConstantBuffers(1, 1, &dcb);
        c->PSSetShaderResources(0, 1, m_cachedSrv.GetAddressOf());
        c->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        c->VSSetShader(m_easuVs.Get(), nullptr, 0);
        c->PSSetShader(m_dbgPs.Get(), nullptr, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->Draw(3, 0);
        c->PSSetShaderResources(0, 1, &nullSrv);
        c->PSSetConstantBuffers(1, 1, &nullCb);
        m_swapchain->Present(m_uncapped ? 0 : 1, 0);
        return;
    }

    // --- EASU: src -> intermediate -------------------------------------------
    ID3D11RenderTargetView* midRtv = m_midRtv.Get();
    c->OMSetRenderTargets(1, &midRtv, nullptr);
    D3D11_VIEWPORT vp{0.0f, 0.0f, float(m_outW), float(m_outH), 0.0f, 1.0f};
    c->RSSetViewports(1, &vp);
    c->RSSetState(m_raster.Get());
    c->PSSetConstantBuffers(0, 1, m_cb.GetAddressOf());
    c->PSSetShaderResources(0, 1, m_cachedSrv.GetAddressOf());
    c->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    c->VSSetShader(m_easuVs.Get(), nullptr, 0);
    c->PSSetShader(m_easuPs.Get(), nullptr, 0);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->Draw(3, 0);

    // --- RCAS: intermediate -> backbuffer ------------------------------------
    // src already unbound by the debug block / pass below; drop it now so the
    // slot is free for the intermediate
    c->PSSetShaderResources(0, 1, &nullSrv);

    c->OMSetRenderTargets(1, backRtv.GetAddressOf(), nullptr);
    c->PSSetShaderResources(0, 1, m_midSrv.GetAddressOf());
    c->VSSetShader(m_rcasVs.Get(), nullptr, 0);
    c->PSSetShader(m_rcasPs.Get(), nullptr, 0);
    c->Draw(3, 0);

    // clean unbind (avoids SRV-bound-as-RTV feedback hazards later)
    c->PSSetShaderResources(0, 1, &nullSrv);

    m_swapchain->Present(m_uncapped ? 0 : 1, 0);
}

void Fsr11Presenter::shutdown()
{
    m_cachedSrv.Reset();
    m_cachedTex.Reset();
    m_raster.Reset();
    m_easuPs.Reset();
    m_easuVs.Reset();
    m_rcasPs.Reset();
    m_rcasVs.Reset();
    m_dbgPs.Reset();
    m_dbgCb.Reset();
    m_sampler.Reset();
    m_cb.Reset();
    m_midSrv.Reset();
    m_midRtv.Reset();
    m_intermediate.Reset();
    m_swapchain.Reset();
    m_ctx.Reset();
    m_device.Reset();
    m_adapter.Reset();
}

} // namespace capture
