#include "poc_app.h"

#include "upscaling/fsr1_upscaler.h"

#include <cstdio>
#include <cwchar>
#include <shellapi.h>

namespace {

const wchar_t kWindowClass[] = L"MinovaUpscalerPoCWindow";
const wchar_t kWindowTitle[] = L"MinovaUpscaler Phase 1 PoC";

/// Adjust a client area to the desired window.
bool createWindow(HWND& out, UINT w, UINT h)
{
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = PocApp::wndProcThunk;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc))
        return false;

    RECT rect{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    out = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT,
                          rect.right - rect.left, rect.bottom - rect.top,
                          nullptr, nullptr, wc.hInstance, nullptr);
    return out != nullptr;
}

std::wstring exeDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos)
        dir.resize(slash);
    return dir;
}

} // namespace

PocApp::PocApp(const PocOptions& opts) : m_opts(opts) {}

PocApp::~PocApp() = default;

LRESULT CALLBACK PocApp::wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return 0;
    }
    auto* self = reinterpret_cast<PocApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self)
        return self->wndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PocApp::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_ESCAPE:
            DestroyWindow(hwnd);
            return 0;
        case 'T': {
            UpscaleMode m = m_renderer.getMode();
            m = (m == UpscaleMode::XeSS)     ? UpscaleMode::Bilinear :
                (m == UpscaleMode::Bilinear) ? UpscaleMode::Fsr      :
                (m == UpscaleMode::Fsr)      ? UpscaleMode::Native   :
                                               UpscaleMode::XeSS;
            m_renderer.setMode(m);
            std::printf("[mode] -> %s\n", UpscaleModeName(m));
            updateTitle();
            return 0;
        }
        case 'P':
            m_renderer.paused = !m_renderer.paused;
            return 0;
        }
        break;
    case WM_SIZE:
        // Fixed-size PoC: ignore resizing beyond the initial layout.
        if (m_renderer.getOutputW() != 0 && wp == SIZE_RESTORED) {
            // not handled: keep it simple for phase 1
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void PocApp::updateTitle()
{
    wchar_t buf[160];
    std::swprintf(buf, sizeof(buf) / sizeof(wchar_t),
                  L"%s | %hs | scene %ux%u -> %ux%u | %.0f fps",
                  kWindowTitle, UpscaleModeName(m_renderer.getMode()),
                  m_renderer.getSceneW(), m_renderer.getSceneH(),
                  m_renderer.getOutputW(), m_renderer.getOutputH(),
                  m_renderer.getCpuFps());
    SetWindowTextW(m_hwnd, buf);
}

int PocApp::run()
{
    if (!createWindow(m_hwnd, m_opts.outputW, m_opts.outputH)) {
        std::fprintf(stderr, "window creation failed\n");
        return 1;
    }

    const std::wstring shaderDir = exeDirectory() + L"\\shaders";
    if (!m_renderer.initialize(m_hwnd, m_opts.outputW, m_opts.outputH,
                               m_opts.quality, m_opts.gpuIndex, m_opts.mode,
                               m_opts.csvPath, shaderDir)) {
        return 2;
    }
    m_renderer.setFsrSharpness(m_opts.sharpness < 0.0f
                                   ? fsr1::kDefaultSharpnessStops
                                   : m_opts.sharpness);

    ShowWindow(m_hwnd, SW_SHOWNORMAL);
    updateTitle();
    m_running = true;

    auto start = GetTickCount64();
    MSG msg{};
    while (m_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { m_running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!m_running)
            break;

        try {
            m_renderer.renderFrame();
        }
        catch (const std::exception& e) {
            std::fprintf(stderr, "[frame] exception: %s\n", e.what());
            std::fflush(stderr);

            // Auto-recover from transient device-removed (the one-time XeSS
            // JIT burst can outlive the driver watchdog on slow iGPUs).
            if (++m_retries <= 3) {
                DWORD delayMs = 2000u << (m_retries - 1);   // 2s, 4s, 8s
                std::fprintf(stderr, "[frame] recreating device in %lu ms "
                                     "(attempt %u/3)\n", delayMs, m_retries);
                std::fflush(stderr);
                Sleep(delayMs);
                m_renderer.shutdown();
                if (m_renderer.initialize(m_hwnd, m_opts.outputW, m_opts.outputH,
                                          m_opts.quality, m_opts.gpuIndex,
                                          m_opts.mode, m_opts.csvPath, shaderDir)) {
                    updateTitle();
                    continue;
                }
                std::fprintf(stderr, "[frame] re-init failed\n");
            }
            m_running = false;
        }

        static uint64_t s_prevTitleTick = 0;
        uint64_t ticks = GetTickCount64();
        if (ticks - s_prevTitleTick >= 500) {
            s_prevTitleTick = ticks;
            updateTitle();
        }

        if (m_opts.runSeconds > 0.0 &&
            (ticks - start) >= static_cast<uint64_t>(m_opts.runSeconds * 1000.0))
            break;
    }

    m_renderer.shutdown();
    DestroyWindow(m_hwnd);
    return 0;
}
