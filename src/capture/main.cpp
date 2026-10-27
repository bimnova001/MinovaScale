// MinovaUpscaler capture-based FSR presenter (Phase 3):
//
//   Renders a game at its own (lower) resolution in a window, captures that
//   window with Windows.Graphics.Capture (no injection, no game access),
//   and reconstructs it with AMD FSR 1 (EASU + RCAS) to a 1080p-class
//   presenter window.
//
// Honest labeling: this is a capture path, not in-game reconstruction.
// It adds a capture latency of roughly one to two frames, and image quality
// is that of FSR 1 (spatial), not a temporal upscaler.

#include "capture_app.h"
#include "upscaling/fsr1_upscaler.h"

#include <windows.h>
#include <shellapi.h>
#include <string>

namespace {

void usage()
{
    std::printf(
        "MinovaUpscaler capture-based FSR presenter\n"
        "usage: minova_capture [options]\n"
        "  --window NAME   visible window title substring to capture (required)\n"
        "  --out W H       presenter size (default 1920x1080)\n"
        "  --force-res W H resize the GAME window to a client size of WxH\n"
        "                  (plain Win32 window management; forces windowed\n"
        "                  engines to render lower-res, e.g. 1280 720)\n"
        "  --overlay       direct-display mode: borderless click-through\n"
        "                  window over the game's original rect (needs WGC;\n"
        "                  falls back to a side window otherwise)\n"
        "  --sharpness X   FSR RCAS stops: 0 = max sharpen, higher = softer\n"
        "                  (default %.1f)\n"
        "  --gpu_id N      DXGI adapter index (default: first Intel adapter)\n"
        "  --seconds S     auto-quit after S seconds (0 = until closed)\n"
        "  --list-windows  print visible window titles and exit\n"
        "  --help          this text\n"
        "\n"
        "Play your game windowed at a reduced resolution (e.g. 960x540) and\n"
        "this presenter shows it at %lux%lu with AMD FSR 1 spatial upscaling.\n",
        fsr1::kDefaultSharpnessStops,
        1920ul, 1080ul);
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    capture::AppOptions opts;
    bool listWindows = false;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        std::fprintf(stderr, "cannot parse command line\n");
        return 1;
    }
    auto ansi = [](const wchar_t* w) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len > 1 ? len - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
        return s;
    };

    for (int i = 1; i < argc; ++i) {
        const std::string k = ansi(argv[i]);
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", k.c_str());
                exit(1);
            }
            return ansi(argv[++i]);
        };
        if      (k == "--help" || k == "-h") { usage(); LocalFree(argv); return 0; }
        else if (k == "--list-windows")      { listWindows = true; }
        else if (k == "--window")            opts.windowTitle = next();
        else if (k == "--pid")               opts.targetPid = std::stoul(next());
        else if (k == "--sharpness")         opts.sharpness = std::stof(next());
        else if (k == "--overlay")           opts.overlay = true;
        else if (k == "--force-res") {
            opts.forceW = std::stoi(next());
            opts.forceH = std::stoi(next());
        }
        else if (k == "--gpu_id")            opts.gpuIndex = std::stoi(next());
        else if (k == "--seconds")           opts.runSeconds = std::stod(next());
        else if (k == "--out") {
            opts.outW = static_cast<unsigned>(std::stoul(next()));
            opts.outH = static_cast<unsigned>(std::stoul(next()));
        }
        else {
            std::fprintf(stderr, "unknown option %s\n", k.c_str());
            usage(); LocalFree(argv);
            return 1;
        }
    }
    LocalFree(argv);

    if (listWindows) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        std::printf("Visible top-level windows:\n");
        return capture::listWindows();
    }

    // Per-monitor DPI: physical-pixel rects must match the WGC texture pixels.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    return capture::runApp(opts);
}
