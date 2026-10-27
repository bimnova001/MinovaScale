#include "wgc_source.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

using Microsoft::WRL::ComPtr;

namespace capture {

namespace {

[[noreturn]] void throwHr(HRESULT hr, const char* what)
{
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s failed (hr=0x%08lX)",
                  what, static_cast<unsigned long>(hr));
    throw std::runtime_error(msg);
}

// Opaque WinRT holder (kept out of the header).
struct WgcState {
    wgdx::Direct3D11::IDirect3DDevice d3dWinrt{nullptr};
    wgc::GraphicsCaptureItem          item{nullptr};
    wgc::Direct3D11CaptureFramePool   pool{nullptr};
    wgc::GraphicsCaptureSession       session{nullptr};
};

} // namespace

void WgcSource::initialize(ComPtr<ID3D11Device> device, HWND targetHwnd)
{
    try {
        m_device = device;
        m_device->GetImmediateContext(m_ctx.GetAddressOf());
        m_target = targetHwnd;

        auto* s = new WgcState();
        m_winrt = s;

        if (!wgc::GraphicsCaptureSession::IsSupported())
            throw std::runtime_error(
                "Windows.Graphics.Capture is not supported on this system");

        // IDirect3DDevice wrapping OUR presenter device (one device total).
        ComPtr<IDXGIDevice> dxgi;
        HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(dxgi.GetAddressOf()));
        if (FAILED(hr)) throwHr(hr, "IDXGIDevice (wgc)");
        winrt::com_ptr<::IInspectable> insp;
        hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put());
        if (FAILED(hr)) throwHr(hr, "CreateDirect3D11DeviceFromDXGIDevice");
        s->d3dWinrt = insp.as<wgdx::Direct3D11::IDirect3DDevice>();

        // capture item for the target window
        auto interop = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        hr = interop->CreateForWindow(
            targetHwnd, winrt::guid_of<wgc::GraphicsCaptureItem>(),
            reinterpret_cast<void**>(winrt::put_abi(item)));
        if (FAILED(hr) || !item) throwHr(hr, "CreateForWindow");
        s->item = item;

        const auto size = item.Size();
        m_poolW = static_cast<unsigned>(size.Width);
        m_poolH = static_cast<unsigned>(size.Height);

        // free-threaded pool (no dispatcher needed); B8G8R8A8 = sRGB desktop
        s->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s->d3dWinrt, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, size);
        s->session = s->pool.CreateCaptureSession(s->item);
        // hide the yellow border if the OS allows (needs package identity on
        // some builds; failure is harmless)
        try { s->session.IsBorderRequired(false); } catch (...) {}
        s->session.StartCapture();

        // stable hold texture at the pool size
        D3D11_TEXTURE2D_DESC td{};
        td.Width      = m_poolW;
        td.Height     = m_poolH;
        td.MipLevels  = 1;
        td.ArraySize  = 1;
        td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc = {1, 0};
        td.Usage      = D3D11_USAGE_DEFAULT;
        td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
        hr = m_device->CreateTexture2D(&td, nullptr, m_hold.GetAddressOf());
        if (FAILED(hr)) throwHr(hr, "wgc hold texture");

        std::printf("[capture] WGC ready: window texture %ux%u\n", m_poolW, m_poolH);
    }
    catch (...) { shutdown(); throw; }
}

void WgcSource::recreatePool(unsigned w, unsigned h)
{
    auto* s = static_cast<WgcState*>(m_winrt);
    if (!s) return;

    const winrt::Windows::Graphics::SizeInt32 size{int(w), int(h)};
    s->pool.Recreate(s->d3dWinrt,
                     wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                     2, size);
    m_poolW = w;
    m_poolH = h;

    // hold texture must match pool frames for CopyResource
    m_hold.Reset();
    D3D11_TEXTURE2D_DESC td{};
    td.Width      = w;
    td.Height     = h;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = m_device->CreateTexture2D(&td, nullptr, m_hold.GetAddressOf());
    if (FAILED(hr)) throwHr(hr, "wgc hold texture (resize)");
}

bool WgcSource::acquireFrame(ComPtr<ID3D11Texture2D>* outTex,
                             unsigned* outW, unsigned* outH)
{
    auto* s = static_cast<WgcState*>(m_winrt);
    if (!s || !m_hold) return false;

    auto frame = s->pool.TryGetNextFrame();
    if (!frame) return false;

    // target window resized since the pool was created?
    const auto content = frame.ContentSize();
    if (static_cast<unsigned>(content.Width)  != m_poolW ||
        static_cast<unsigned>(content.Height) != m_poolH) {
        recreatePool(static_cast<unsigned>(content.Width),
                     static_cast<unsigned>(content.Height));
    }

    // frame surface -> stable hold texture
    auto access = frame.Surface().as<
        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> frameTex;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(frameTex.GetAddressOf())));
    m_ctx->CopyResource(m_hold.Get(), frameTex.Get());

    *outTex = m_hold;
    *outW = m_poolW;
    *outH = m_poolH;
    return true;
}

void WgcSource::shutdown()
{
    if (m_winrt) {
        auto* s = static_cast<WgcState*>(m_winrt);
        s->session = nullptr;
        s->pool = nullptr;
        s->item = nullptr;
        s->d3dWinrt = nullptr;
        delete s;
        m_winrt = nullptr;
    }
    m_hold.Reset();
    m_ctx.Reset();
    m_device.Reset();
}

} // namespace capture
