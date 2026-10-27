#include "dda_source.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace capture {

namespace {

void throwHr(HRESULT hr, const char* what)
{
    char msg[256];
    std::snprintf(msg, sizeof(msg), "%s failed (hr=0x%08lX)",
                  what, static_cast<unsigned long>(hr));
    throw std::runtime_error(msg);
}

} // namespace

void DdaSource::initialize(ComPtr<ID3D11Device> device, HWND targetHwnd)
{
    try {
        m_device = device;
        m_device->GetImmediateContext(m_ctx.GetAddressOf());
        m_target = targetHwnd;

        // monitor that contains the target window
        HMONITOR mon = MonitorFromWindow(targetHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi))
            throw std::runtime_error("GetMonitorInfo failed");
        m_monX = mi.rcMonitor.left;
        m_monY = mi.rcMonitor.top;
        m_monW = static_cast<unsigned>(mi.rcMonitor.right  - mi.rcMonitor.left);
        m_monH = static_cast<unsigned>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        std::printf("[capture] DDA monitor: %d,%d %ux%u\n",
                    m_monX, m_monY, m_monW, m_monH);

        recreateDuplication();

        std::printf("[capture] DDA ready: desktop %ux%u\n", m_monW, m_monH);
    }
    catch (...) { shutdown(); throw; }
}

void DdaSource::recreateDuplication()
{
    if (!m_device || !m_ctx) return;

    // find the DXGI output matching the target's monitor
    ComPtr<IDXGIDevice> dxgiDev;
    HRESULT hr = m_device->QueryInterface(IID_PPV_ARGS(dxgiDev.GetAddressOf()));
    if (FAILED(hr)) throwHr(hr, "IDXGIDevice (DDA)");
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDev->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) throwHr(hr, "GetAdapter (DDA)");

    ComPtr<IDXGIOutput> chosen;
    for (UINT i = 0;
         adapter->EnumOutputs(i, chosen.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_OUTPUT_DESC od{};
        if (FAILED(chosen->GetDesc(&od))) continue;
        if (od.DesktopCoordinates.left == m_monX &&
            od.DesktopCoordinates.top  == m_monY &&
            od.DesktopCoordinates.right  - od.DesktopCoordinates.left ==
                LONG(m_monW) &&
            od.DesktopCoordinates.bottom - od.DesktopCoordinates.top  ==
                LONG(m_monH)) {
            break;   // chosen holds the matching output
        }
    }
    if (!chosen) throw std::runtime_error("no DXGI output for the target monitor");

    ComPtr<IDXGIOutput1> out1;
    hr = chosen->QueryInterface(IID_PPV_ARGS(out1.GetAddressOf()));
    if (FAILED(hr)) throwHr(hr, "IDXGIOutput1");
    m_dup.Reset();
    hr = out1->DuplicateOutput(m_device.Get(), m_dup.GetAddressOf());
    if (FAILED(hr)) throwHr(hr, "DuplicateOutput");

    // stable hold texture at the desktop size
    if (m_hold) return;    // same size; keep it (size never changes mid-session)
    DXGI_OUTDUPL_DESC dd{};
    m_dup->GetDesc(&dd);
    m_monW = static_cast<unsigned>(dd.ModeDesc.Width);
    m_monH = static_cast<unsigned>(dd.ModeDesc.Height);

    D3D11_TEXTURE2D_DESC td{};
    td.Width      = m_monW;
    td.Height     = m_monH;
    td.MipLevels  = 1;
    td.ArraySize  = 1;
    td.Format     = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc = {1, 0};
    td.Usage      = D3D11_USAGE_DEFAULT;
    td.BindFlags  = D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&td, nullptr, m_hold.GetAddressOf());
    if (FAILED(hr)) throwHr(hr, "DDA hold texture");
}

bool DdaSource::acquireFrame(ComPtr<ID3D11Texture2D>* outTex,
                             unsigned* outW, unsigned* outH)
{
    if (!m_dup || !m_hold) {
        // duplication died (mode change / TDR) - recreate quietly
        try { recreateDuplication(); }
        catch (...) { Sleep(200); return false; }
    }

    ComPtr<IDXGIResource> frame;
    DXGI_OUTDUPL_FRAME_INFO info{};
    HRESULT hr = m_dup->AcquireNextFrame(8, &info, frame.GetAddressOf());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        m_dup.Reset();
        Sleep(200);
        return false;
    }
    bool produced = (info.LastPresentTime.QuadPart != 0);

    if (produced) {
        ComPtr<ID3D11Texture2D> tex11;
        if (FAILED(frame->QueryInterface(IID_PPV_ARGS(tex11.GetAddressOf())))) {
            produced = false;   // unexpected; skip this frame
        } else {
            m_ctx->CopyResource(m_hold.Get(), tex11.Get());
        }
    }
    m_dup->ReleaseFrame();

    if (!produced) return false;
    *outTex = m_hold;
    *outW = m_monW;
    *outH = m_monH;
    return true;
}

void DdaSource::shutdown()
{
    m_hold.Reset();
    m_dup.Reset();
    m_ctx.Reset();
    m_device.Reset();
}

} // namespace capture
