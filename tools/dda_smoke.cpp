// tools/dda_smoke.cpp — Desktop Duplication API sanity check (direct DXGI).
// If this works while WGC AVs, the compositing stack is alive and only the
// Windows.Graphics.Capture service is broken on this build.

#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <cstdio>
#include <wrl/client.h>

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   dev.GetAddressOf(), &fl, ctx.GetAddressOf());
    if (FAILED(hr)) { std::printf("device failed 0x%08lX\n", (unsigned long)hr); return 2; }

    Microsoft::WRL::ComPtr<IDXGIDevice1> dxgiDev;
    dev->QueryInterface(IID_PPV_ARGS(dxgiDev.GetAddressOf()));
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    dxgiDev->GetAdapter(adapter.GetAddressOf());
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, output.GetAddressOf()))) {
        std::printf("no output\n"); return 2;
    }
    Microsoft::WRL::ComPtr<IDXGIOutput1> out1;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(out1.GetAddressOf())))) {
        std::printf("output1 failed\n"); return 2;
    }

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> dup;
    hr = out1->DuplicateOutput(dev.Get(), dup.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("DuplicateOutput failed 0x%08lX (hr)\n", (unsigned long)hr);
        return 2;
    }

    Microsoft::WRL::ComPtr<IDXGIResource> res;
    DXGI_OUTDUPL_FRAME_INFO info{};
    hr = dup->AcquireNextFrame(2000, &info, res.GetAddressOf());
    if (FAILED(hr)) {
        std::printf("AcquireNextFrame failed 0x%08lX\n", (unsigned long)hr);
        return 2;
    }
    dup->ReleaseFrame();
    std::printf("DDA SUCCESS: accumulatedFrames=%lu lastPresent=%lld\n",
                (unsigned long)info.AccumulatedFrames,
                (long long)info.LastPresentTime.QuadPart);
    return 0;
}
