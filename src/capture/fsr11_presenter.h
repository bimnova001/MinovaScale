#pragma once

// FSR 1.0 presenter for the capture tool - D3D11-only.
//
// The whole capture path lives in D3D11 natively (WGC frame pools and Desktop
// Duplication are D3D11 APIs), so doing the upscaling in the SAME D3D11 device
// removes all cross-device plumbing (shared handles, fences, 11on12, D3D12
// barrier states) that the earlier D3D12 presenter needed. The immediate
// context serializes capture copy -> EASU -> RCAS -> Present by itself.
//
// Pipeline per frame (fullscreen triangle, no vertex buffers):
//   src texture (BGRA, captured) -> EASU -> intermediate (BGRA)
//   intermediate -> RCAS -> swapchain backbuffer (BGRA)

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>

struct HWND__;
typedef struct HWND__* HWND;

namespace capture {

class Fsr11Presenter {
public:
    Fsr11Presenter() = default;
    ~Fsr11Presenter() { shutdown(); }
    Fsr11Presenter(const Fsr11Presenter&) = delete;
    Fsr11Presenter& operator=(const Fsr11Presenter&) = delete;

    void initialize(HWND presenterHwnd, unsigned outW, unsigned outH,
                    float sharpnessStops, int requestedGpuIndex,
                    const std::wstring& shaderDir,
                    Microsoft::WRL::ComPtr<ID3D11Device>* deviceOut);

    unsigned outW() const { return m_outW; }
    unsigned outH() const { return m_outH; }

    /// Draws EASU+RCAS from srcTex (BGRA) into the current backbuffer and
    /// presents. The crop (in source pixels) selects the client area.
    void render(ID3D11Texture2D* srcTex, unsigned srcW, unsigned srcH,
                unsigned cropX, unsigned cropY,
                unsigned cropW, unsigned cropH);

    void shutdown();

private:
    Microsoft::WRL::ComPtr<ID3DBlob> compileShader(const std::wstring& file,
                                                   const char* entry,
                                                   const char* target) const;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_ctx;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>        m_swapchain;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_intermediate;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_midRtv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_midSrv;

    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_cb;      // DYNAMIC constants
    Microsoft::WRL::ComPtr<ID3D11SamplerState>     m_sampler;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState>  m_raster;
    Microsoft::WRL::ComPtr<ID3D11VertexShader>     m_easuVs, m_rcasVs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_easuPs, m_rcasPs;

    // debug quadrant shader (MINOVA_DEBUG_SHADER); see fsr11_dbg.ps below
    Microsoft::WRL::ComPtr<ID3D11PixelShader>      m_dbgPs;
    Microsoft::WRL::ComPtr<ID3D11Buffer>           m_dbgCb;

    // cached SRV of the (usually stable) capture texture
    Microsoft::WRL::ComPtr<ID3D11Texture2D>          m_cachedTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cachedSrv;

    unsigned m_outW = 0, m_outH = 0;
    float    m_sharpness = 0.2f;
};

} // namespace capture
