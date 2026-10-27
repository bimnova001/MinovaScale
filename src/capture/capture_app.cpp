#include "capture_app.h"

#include <windows.h>

#include "fsr11_presenter.h"
#include "wgc_source.h"
#include "dda_source.h"

#include <dwmapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

#include <winrt/base.h>

#include "upscaling/fsr1_upscaler.h"

using Microsoft::WRL::ComPtr;

namespace capture {

namespace {

const wchar_t kWindowClass[]   = L"MinovaUpscalerCaptureWindow";
const wchar_t kOverlayClass[]  = L"MinovaUpscalerOverlayWindow";

std::string narrowUtf8(const wchar_t* w)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len > 0 ? len - 1 : 0), '\0');
    if (len > 0)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::string toLowerA(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// --list-windows / GUI picker: collect every visible top-level window title
BOOL CALLBACK enumCollectProc(HWND hwnd, LPARAM lp)
{
    auto* v = reinterpret_cast<std::vector<WindowEntry>*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    wchar_t buf[256] = {};
    if (GetWindowTextW(hwnd, buf, 256) <= 0) return TRUE;
    v->push_back(WindowEntry{(unsigned long long)(uintptr_t)hwnd,
                             (unsigned long)pid,
                             std::wstring(buf)});
    return TRUE;
}

std::wstring exeDirW()
{
    wchar_t path[MAX_PATH + 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    size_t slash = dir.find_last_of(L"/\\");
    if (slash != std::wstring::npos) dir.resize(slash);
    return dir;
}

/// Health probe for the Windows.Graphics.Capture stack. Spawns the bundled
/// wgc_smoke.exe child (isolated: if the OS build crashes WGC pool creation,
/// the child dies, not us) and checks its exit code.
bool probeWgcHealthy()
{
    std::wstring smoke = exeDirW() + L"\\wgc_smoke.exe";
    std::wstring cmd = L"\"" + smoke + L"\" monitor 1";

    // stdout -> NUL keeps the console clean
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (nul) {
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = nul;
        si.hStdError  = nul;
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                             nul ? TRUE : FALSE, CREATE_NO_WINDOW,
                             nullptr, nullptr, &si, &pi);
    if (nul) CloseHandle(nul);
    if (!ok) return false;

    const DWORD wait = WaitForSingleObject(pi.hProcess, 6000);
    bool healthy = false;
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        healthy = (code == 0);
    } else {
        TerminateProcess(pi.hProcess, 1);      // hung
        WaitForSingleObject(pi.hProcess, 1000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return healthy;
}

struct EnumWindowsCtx {
    const std::string* lower = nullptr;   // lowercase pattern
    HWND               exact = nullptr;    // full-title == pattern
    HWND               fuzzy = nullptr;    // title contains pattern
};

BOOL CALLBACK enumFindProc(HWND hwnd, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumWindowsCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;

    int cloaked = 0;
    DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;

    // never capture our own presenter windows
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    wchar_t buf[256] = {};
    if (GetWindowTextW(hwnd, buf, 256) <= 0) return TRUE;
    std::string title = toLowerA(narrowUtf8(buf));

    if (title == *ctx->lower) { ctx->exact = hwnd; return FALSE; }
    if (!ctx->fuzzy &&
        title.find(*ctx->lower) != std::string::npos) {
        ctx->fuzzy = hwnd;
    }
    return TRUE;
}

LRESULT CALLBACK presenterWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE || wp == 'Q') DestroyWindow(hwnd);
        return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Overlay window never takes focus or input; clicks fall through to the game.
LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

struct ClientCrop {
    unsigned x = 0, y = 0;        // client-area offset inside the window rect
    unsigned w = 0, h = 0;        // client area size (physical px)
};

ClientCrop queryClientCrop(HWND hwnd, unsigned frameW, unsigned frameH)
{
    // Client-area crop: the frame texture is the whole window incl. borders
    // (WGC window capture). Client size is mapped into texture-pixel space
    // via the DWM extended frame bounds (which WGC uses, excluding shadows).
    ClientCrop c{};
    RECT wr{}, cr{};
    if (!GetWindowRect(hwnd, &wr) || !GetClientRect(hwnd, &cr)) return c;
    POINT tl{0, 0};
    ClientToScreen(hwnd, &tl);
    LONG offX = tl.x - wr.left;          // border + non-client width
    LONG offY = tl.y - wr.top;           // title bar height
    LONG winW = wr.right - wr.left;
    LONG winH = wr.bottom - wr.top;
    if (winW <= 0 || winH <= 0) return c;

    RECT bounds{};
    if (SUCCEEDED(DwmGetWindowAttribute(
            hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) {
        LONG dwW = bounds.right - bounds.left;
        LONG dwH = bounds.bottom - bounds.top;
        if (dwW > 0 && dwH > 0) {
            // WGC texture size corresponds to the DWM frame bounds.
            const double sx = double(frameW) / double(dwW);
            const double sy = double(frameH) / double(dwH);
            offX = LONG(double(offX) * sx);
            offY = LONG(double(offY) * sy);
            winW = dwW;
            winH = dwH;
        }
    }

    c.x = offX > 0 ? unsigned(offX) : 0u;
    c.y = offY > 0 ? unsigned(offY) : 0u;
    c.w = unsigned(double(cr.right));
    c.h = unsigned(double(cr.bottom));
    if (winW > 0 && c.w > unsigned(winW)) c.w = unsigned(winW);
    if (winH > 0 && c.h > unsigned(winH)) c.h = unsigned(winH);
    if (c.w > frameW) c.w = frameW;
    if (c.h > frameH) c.h = frameH;
    return c;
}

/// Desktop-duplication crop: the target's DWM extended frame bounds are
/// physical pixels regardless of the target's DPI awareness, which makes
/// them safe inside the physical desktop texture. (GetClientRect of a
/// DPI-unaware target is virtualized and MUST NOT be used here.)
ClientCrop queryDesktopCrop(HWND hwnd, int monX, int monY, int monW, int monH)
{
    ClientCrop c{};
    RECT b{};
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &b, sizeof(b)))) {
        GetWindowRect(hwnd, &b);
    }
    // clip to the monitor rect (DDA texture covers exactly the monitor)
    LONG x0 = b.left - monX, y0 = b.top - monY;
    LONG x1 = b.right - monX, y1 = b.bottom - monY;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LONG(monW)) x1 = LONG(monW);
    if (y1 > LONG(monH)) y1 = LONG(monH);
    c.x = x0 > 0 ? unsigned(x0) : 0u;
    c.y = y0 > 0 ? unsigned(y0) : 0u;
    c.w = unsigned((x1 - x0) > 0 ? (x1 - x0) : 0);
    c.h = unsigned((y1 - y0) > 0 ? (y1 - y0) : 0);
    return c;
}

/// DWM frame bounds in physical screen coordinates (safe for DPI-unaware
/// targets, unlike GetClientRect virtualization).
bool queryPhysicalBounds(HWND hwnd, RECT* out)
{
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        out, sizeof(*out))))
        return true;
    return GetWindowRect(hwnd, out) != FALSE;
}

/// Force a window to have the requested CLIENT size by adjusting the outer
/// size for its current style (plain Win32 window management - no injection).
void forceWindowSize(HWND hwnd, int clientW, int clientH)
{
    const LONG style   = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    RECT need{0, 0, clientW, clientH};
    AdjustWindowRectEx(&need, static_cast<DWORD>(style), FALSE,
                       static_cast<DWORD>(exStyle));
    const int outerW = need.right - need.left;
    const int outerH = need.bottom - need.top;
    SetWindowPos(hwnd, nullptr, 0, 0, outerW, outerH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_NOOWNERZORDER);
    std::printf("[capture] force-res: client %dx%d (outer %dx%d)\n",
                clientW, clientH, outerW, outerH);
}

} // anonymous namespace

int listWindows()
{
    for (const auto& e : listCaptureTargets())
        std::printf("  0x%llx | %s\n", e.hwnd,
                    narrowUtf8(e.title.c_str()).c_str());
    return 0;
}

std::vector<WindowEntry> listCaptureTargets()
{
    std::vector<WindowEntry> v;
    EnumWindows(enumCollectProc, reinterpret_cast<LPARAM>(&v));
    return v;
}

int runApp(const AppOptions& opts)
{
    // Multi-threaded WinRT apartment: WGC frame pools are free-threaded and
    // event callbacks arrive on arbitrary threads.
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (...) {
        std::fprintf(stderr, "winrt::init_apartment failed\n");
        return 1;
    }

    std::string titleLower = toLowerA(opts.windowTitle);

    // 1) find the target game window (exact title beats substring)
    HWND target = nullptr;
    if (opts.targetPid) {
        struct PidCtx { DWORD pid = 0; HWND match = nullptr; };
        PidCtx pctx{opts.targetPid, nullptr};
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<PidCtx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && IsWindowVisible(hwnd)) {
                int cloaked = 0;
                DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                      sizeof(cloaked));
                if (cloaked) return TRUE;
                c->match = hwnd;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&pctx));
        target = pctx.match;
        if (!target) {
            std::fprintf(stderr, "no visible window with pid %lu\n",
                         static_cast<unsigned long>(opts.targetPid));
            return 1;
        }
    }
    else {
        EnumWindowsCtx ctx{&titleLower, nullptr, nullptr};
        EnumWindows(enumFindProc, reinterpret_cast<LPARAM>(&ctx));
        target = ctx.exact ? ctx.exact : ctx.fuzzy;
    }
    if (!target) {
        std::fprintf(stderr,
                     "no visible window matches --window \"%s\" "
                     "(run with --list-windows)\n", opts.windowTitle.c_str());
        return 1;
    }

    {
        DWORD pid = 0;
        GetWindowThreadProcessId(target, &pid);
        wchar_t t[256] = {};
        GetWindowTextW(target, t, 256);
        std::printf("[capture] target window: %s (pid %lu)\n",
                    narrowUtf8(t).c_str(), static_cast<unsigned long>(pid));
    }

    // A minimized target renders nothing (DDA) and its DWM bounds point at
    // the iconic position; restore it so capture gets real content.
    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
        Sleep(150);
        std::printf("[capture] target was minimized - restored\n");
    }

    // Remember the game's original bounds so we can restore it on exit and
    // cover it 1:1 with the overlay.
    RECT origBounds{};
    queryPhysicalBounds(target, &origBounds);
    const int origW = origBounds.right - origBounds.left;
    const int origH = origBounds.bottom - origBounds.top;

    // 2) --force-res: shrink the game's client area so windowed engines
    //    render at the reduced resolution (plain SetWindowPos - allowed).
    if (opts.forceW > 0 && opts.forceH > 0) {
        forceWindowSize(target, opts.forceW, opts.forceH);
        Sleep(150);   // let the engine pick up the new size
    }

    // 3) capture backend: WGC (probed in an isolated child) or Desktop
    //    Duplication fallback.
    const bool wgcOk = probeWgcHealthy();
    const char* backendName = wgcOk
        ? "Windows.Graphics.Capture" : "Desktop Duplication";
    if (wgcOk)
        std::printf("[capture] backend: %s\n", backendName);
    else
        std::printf("[capture] backend: %s (WGC unavailable on this build)\n",
                    backendName);

    // Overlay = direct-display mode (needs WGC: captures through occlusion).
    // DDA sees what is ON SCREEN, so with DDA the presenter is a side window.
    const bool overlay = opts.overlay && wgcOk;
    unsigned outW = opts.outW;
    unsigned outH = opts.outH;
    if (overlay) {
        // The overlay covers the game 1:1, so match its original bounds.
        outW = unsigned(origW);
        outH = unsigned(origH);
        std::printf("[capture] mode: overlay %ux%u over the game window "
                    "(click-through; Esc via console Ctrl+C to quit)\n",
                    outW, outH);
    }
    else {
        if (opts.overlay)
            std::printf("[capture] mode: overlay requested but unavailable "
                        "without WGC - using a side window\n");
        std::printf("[capture] mode: side window %ux%u\n", outW, outH);
    }

    // 4) presenter window
    HWND presenterHwnd = nullptr;
    {
        WNDCLASSEXW wcx{};
        wcx.cbSize        = sizeof(wcx);
        wcx.style         = CS_HREDRAW | CS_VREDRAW;
        wcx.lpfnWndProc   = overlay ? overlayWndProc : presenterWndProc;
        wcx.hInstance     = GetModuleHandleW(nullptr);
        wcx.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wcx.lpszClassName = overlay ? kOverlayClass : kWindowClass;
        if (!RegisterClassExW(&wcx)) {
            std::fprintf(stderr, "RegisterClass failed\n");
            return 1;
        }

        if (overlay) {
            const DWORD ex =
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            presenterHwnd = CreateWindowExW(
                ex, kOverlayClass, L"MinovaUpscaler",
                WS_POPUP,
                origBounds.left, origBounds.top, origW, origH,
                nullptr, nullptr, wcx.hInstance, nullptr);
            if (!presenterHwnd) {
                std::fprintf(stderr, "overlay window creation failed\n");
                return 1;
            }
            // fully opaque but layered: enables WS_EX_TRANSPARENT
            // click-through semantics while rendering at full alpha
            SetLayeredWindowAttributes(presenterHwnd, 0, 255, LWA_ALPHA);
            ShowWindow(presenterHwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(presenterHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        else {
            RECT rect{0, 0, LONG(outW), LONG(outH)};
            AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
            CreateWindowExW(0, kWindowClass, L"MinovaUpscaler capture",
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            rect.right - rect.left, rect.bottom - rect.top,
                            nullptr, nullptr, wcx.hInstance, nullptr);
            presenterHwnd = FindWindowW(kWindowClass, nullptr);
            if (!presenterHwnd) {
                std::fprintf(stderr, "presenter window creation failed\n");
                return 1;
            }
            ShowWindow(presenterHwnd, SW_SHOWNORMAL);
        }
    }

    // 5) presenter + capture source (one D3D11 device for everything)
    enum class Backend { Wgc, Dda };
    Backend backend = Backend::Dda;
    Fsr11Presenter presenter;
    WgcSource      wgcSource;
    DdaSource      ddaSource;
    ComPtr<ID3D11Device> device;
    try {
        presenter.initialize(presenterHwnd, outW, outH,
                             opts.sharpness < 0.0f ? fsr1::kDefaultSharpnessStops
                                                   : opts.sharpness,
                             opts.gpuIndex,
                             exeDirW() + L"\\shaders",
                             opts.uncapped, &device);
        // In overlay mode the side presenter window must be kept away from
        // the captured monitor area - DDA would capture the overlay itself.
        if (wgcOk) {
            wgcSource.initialize(device, target);
            backend = Backend::Wgc;
        }
        else {
            ddaSource.initialize(device, target);
            backend = Backend::Dda;
        }
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 2;
    }

    // 6) frame loop
    ComPtr<ID3D11Texture2D> lastTex;
    unsigned srcW = 0, srcH = 0;
    unsigned fpsFrames = 0;
    double   fpsShown = 0.0;
    auto fpsT0 = GetTickCount64();
    auto quitAt = opts.runSeconds > 0.0
                      ? GetTickCount64() + unsigned(opts.runSeconds * 1000.0)
                      : 0;
    std::printf("[capture] running - quit with Esc/Q (side window) or "
                "close the target window\n");

    int exitCode = 0;
    bool running = true;
    unsigned fpsPrintGate = 0;
    while (running) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        if (quitAt && GetTickCount64() > quitAt) break;

        // target closed?
        if (!IsWindow(target)) {
            std::printf("[capture] target window closed - exiting\n");
            break;
        }

        ComPtr<ID3D11Texture2D> tex;
        unsigned w = 0, h = 0;
        bool got = false;
        if (backend == Backend::Wgc) {
            got = wgcSource.acquireFrame(&tex, &w, &h);
        }
        else {
            got = ddaSource.acquireFrame(&tex, &w, &h);
        }
        if (got && tex) {
            lastTex = tex;
            srcW = w;
            srcH = h;
        }

        if (!lastTex) { Sleep(2); continue; }        // no frame yet

        // WGC frames contain the whole window (crop to the client area);
        // DDA frames cover a monitor (crop to the target rect within it).
        ClientCrop crop{};
        if (backend == Backend::Wgc) {
            crop = queryClientCrop(target, srcW, srcH);
        }
        else {
            int mx = 0, my = 0;
            unsigned mw = 0, mh = 0;
            ddaSource.monitorRect(&mx, &my, &mw, &mh);
            crop = queryDesktopCrop(target, mx, my, int(mw), int(mh));
        }
        if (std::getenv("MINOVA_DEBUG") && ++fpsPrintGate == 20) {
            RECT b{};
            DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS,
                                  &b, sizeof(b));
            std::printf("[crop] crop %u,%u %ux%u | dwm bounds L=%ld T=%ld "
                        "R=%ld B=%ld\n", crop.x, crop.y, crop.w, crop.h,
                        b.left, b.top, b.right, b.bottom);
        }
        presenter.render(lastTex.Get(), srcW, srcH,
                         crop.x, crop.y, crop.w, crop.h);

        // fps: newline stream for the GUI/telemetry + title in side-window mode
        ++fpsFrames;
        const uint64_t now = GetTickCount64();
        if (now - fpsT0 >= 500) {
            fpsShown = double(fpsFrames) * 1000.0 /
                       double(now - fpsT0);
            fpsFrames = 0;
            fpsT0 = now;
            std::printf("[fps] %.0f\n", fpsShown);
            if (!overlay) {
                wchar_t t[256];
                std::swprintf(t, 256,
                              L"MinovaUpscaler capture | fps %.0f",
                              fpsShown);
                SetWindowTextW(presenterHwnd, t);
            }
        }
    }

    presenter.shutdown();
    if (backend == Backend::Wgc) wgcSource.shutdown();
    else                          ddaSource.shutdown();

    // restore the game window size/position it had before we touched it
    if (IsWindow(target) && opts.forceW > 0 && opts.forceH > 0) {
        SetWindowPos(target, nullptr,
                     origBounds.left, origBounds.top, origW, origH,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    return exitCode;
}

} // namespace capture
