// tools/wgc_smoke.cpp — focused WGC sanity check.
// No Minova logic: just WinRT init, D3D11, WGC window capture, and a frame
// loop. Built as its own target so any crash here is machine- or
// Windows-API-level, not from our D3D12 presenter.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
#include <d3d11.h>
// coremessaging.h is not shipped in this SDK — declare the minimal ABI here.
enum DQTAT { DQTAT_COM_STA = 0, DQTAT_COM_ASTA = 1, DQTAT_COM_NONE = 2 };
enum DQFT { DQFTC1 = 0 };
enum DQTT { DQ_THREADTYPE_TOP_LEVEL = 0, DQ_THREADTYPE_CURRENT = 1 };
typedef struct tagDispatcherQueueOptions {
    DWORD dwSize;
    int   threadType;
    int   apartmentType;
} DispatcherQueueOptions;
extern "C" HRESULT __stdcall CreateDispatcherQueueController(
    DispatcherQueueOptions options, REFIID riid, void** controller);
#include <wrl/client.h>

namespace wgc  = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;

namespace {

struct PidCtx { DWORD pid = 0; HWND match = nullptr; };

BOOL CALLBACK enumByPid(HWND hwnd, LPARAM lp)
{
    auto* p = reinterpret_cast<PidCtx*>(lp);
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == p->pid && IsWindowVisible(hwnd)) { p->match = hwnd; return FALSE; }
    return TRUE;
}

} // namespace

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc < 2) { std::fprintf(stderr, "usage: wgc_smoke <pid|monitor> [seconds]\n"); return 1; }
    const bool monitorMode = std::strcmp(argv[1], "monitor") == 0;
    const bool dispMode = argc > 3 && std::strcmp(argv[3], "dispatcher") == 0;
    const DWORD pid = monitorMode ? 0 : static_cast<DWORD>(std::strtoul(argv[1], nullptr, 0));
    const int   seconds = argc > 2 ? std::atoi(argv[2]) : 5;

    try {
        std::fprintf(stderr, "S1: init_apartment (MTA)\n");
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        std::fprintf(stderr, "S2: D3D11CreateDevice\n");
        Microsoft::WRL::ComPtr<ID3D11Device> dev;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_FLAG(D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                                     D3D11_CREATE_DEVICE_VIDEO_SUPPORT),
            nullptr, 0, D3D11_SDK_VERSION,
            dev.GetAddressOf(), &fl, ctx.GetAddressOf());
        if (FAILED(hr)) {
            std::fprintf(stderr, "   device failed 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return 2;
        }

        std::fprintf(stderr, "S3: CreateDirect3D11DeviceFromDXGIDevice\n");
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        dev->QueryInterface(IID_PPV_ARGS(dxgiDev.GetAddressOf()));
        IInspectable* insp = nullptr;
        hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDev.Get(), &insp);
        if (FAILED(hr)) {
            std::fprintf(stderr, "   failed 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return 2;
        }
        auto d3dDev = winrt::Windows::Graphics::DirectX::Direct3D11::
            IDirect3DDevice(insp, winrt::take_ownership_from_abi_t{});
        insp->Release();

        std::fprintf(stderr, "S4: find target window by pid %lu\n",
                     static_cast<unsigned long>(pid));
        PidCtx c{pid, nullptr};
        if (!monitorMode) EnumWindows(enumByPid, reinterpret_cast<LPARAM>(&c));
        if (!monitorMode && !c.match) { std::fprintf(stderr, "   no visible window for pid\n"); return 2; }
        if (!monitorMode) {
            wchar_t t[256] = {};
            GetWindowTextW(c.match, t, 256);
            std::fprintf(stderr, "   target: %ls\n", t);
        }

        std::fprintf(stderr, "S5: CreateForWindow\n");
        auto interop = winrt::get_activation_factory<
            wgc::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        wgc::GraphicsCaptureItem item{nullptr};
        if (monitorMode) {
            std::fprintf(stderr, "S4m: TryCreateForMonitor\n");
            POINT p0{0, 0};
            HMONITOR mon = MonitorFromPoint(p0, MONITOR_DEFAULTTOPRIMARY);
            hr = interop->CreateForMonitor(mon, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                           winrt::put_abi(item));
        } else {
            hr = interop->CreateForWindow(c.match, winrt::guid_of<wgc::GraphicsCaptureItem>(),
                                          winrt::put_abi(item));
        }
        if (FAILED(hr) || !item) {
            std::fprintf(stderr, "   failed 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return 2;
        }

        std::fprintf(stderr, "S6: frame pool\n");
        wgc::Direct3D11CaptureFramePool pool{nullptr};
        if (dispMode) {
            std::fprintf(stderr, "    dispatcher-queue path: CreateDispatcherQueueController\n");
            Microsoft::WRL::ComPtr<IUnknown> controller;
            DispatcherQueueOptions options{};
            options.dwSize        = sizeof(options);
            options.threadType    = DQ_THREADTYPE_CURRENT;
            options.apartmentType = DQTAT_COM_ASTA;
            hr = ::CreateDispatcherQueueController(options, IID_PPV_ARGS(controller.GetAddressOf()));
            if (FAILED(hr)) {
                std::fprintf(stderr, "    controller failed 0x%08lX\n", (unsigned long)hr);
                return 2;
            }
            pool = wgc::Direct3D11CaptureFramePool::Create(
                d3dDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        } else {
            pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                d3dDev, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        }
        std::fprintf(stderr, "S7: CreateCaptureSession\n");
        auto session = pool.CreateCaptureSession(item);
        std::fprintf(stderr, "S8: StartCapture\n");
        session.StartCapture();

        std::fprintf(stderr, "S9: frame loop %d s\n", seconds);
        // pump messages (dispatcher pools require it); poll for frames
        LARGE_INTEGER t0{}, t1{}, freq{};
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        int frames = 0;
        while (true) {
            MSG m{};
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            QueryPerformanceCounter(&t1);
            double el = double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart);
            if (el >= double(seconds)) break;
            auto frame = pool.TryGetNextFrame();
            if (frame) {
                ++frames;
                if (frames == 1 || frames % 120 == 0)
                    std::fprintf(stderr, "   frame %d @ %.0f ms\n", frames, el * 1000.0);
            }
            Sleep(4);
        }
        std::fprintf(stderr, "S10: got %d frames -> SUCCESS\n", frames);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "EXC: %s\n", e.what()); return 3; }
    catch (...) { std::fprintf(stderr, "EXC: unknown\n"); return 3; }
}
