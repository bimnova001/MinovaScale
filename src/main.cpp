// MinovaUpscaler — Phase 1 proof of concept entry point.
//
// Renders a controlled D3D12 scene at a lower internal resolution and
// reconstructs it to the output resolution using Intel XeSS-SR, with
// bilinear and native A/B control modes. GPU timings are appended to a
// CSV file (see --csv).
//
// Window controls: T cycles mode, P pauses animation, Esc quits.

#include "app/poc_app.h"
#include "graphics/d3d12_renderer.h"
#include "intel/adapter_selector.h"
#include "upscaling/xess_upscaler.h"

#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

void usage()
{
    std::printf(
        "MinovaUpscaler Phase 1 PoC\n"
        "usage: minova_poc [options]\n"
        "  --mode X        xess | bilinear | fsr | native (default: xess)\n"
        "                  (fsr = AMD FSR 1 spatial upscale, no temporal info)\n"
        "  --quality Q     ultra_performance | performance | balanced | quality |"
        " ultra_quality | ultra_quality_plus | aa  (default: balanced)\n"
        "  --width W       output width,  default 1920\n"
        "  --height H      output height, default 1080\n"
        "  --gpu_id N      DXGI adapter index (default: first Intel adapter)\n"
        "  --csv PATH      telemetry CSV path (default: minova_poc_telemetry.csv)\n"
        "  --seconds S     auto-quit after S seconds (0 = run until closed)\n"
        "  --sharpness X   FSR RCAS sharpness stops (0=max sharpen, higher=softer;\n"
        "                  default 0.2)\n"
        "  --list-adapters print DXGI adapters and exit\n"
        "  --help          this text\n");
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    // Unbuffered console output: keep diagnostics even on hard abort().
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    PocOptions opts;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        std::fprintf(stderr, "cannot parse command line\n");
        return 1;
    }

    auto ansi = [](const wchar_t* w) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(len > 1 ? len - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
        return s;
    };

    bool listAdapters = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = ansi(argv[i]);
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); exit(1); }
            return ansi(argv[++i]);
        };
        if      (k == "--help" || k == "-h")  { usage(); LocalFree(argv); return 0; }
        else if (k == "--list-adapters")      { listAdapters = true; }
        else if (k == "--mode") {
            if (!UpscaleModeFromString(next(), opts.mode)) {
                std::fprintf(stderr, "invalid --mode\n"); usage(); return 1;
            }
        }
        else if (k == "--quality")  opts.quality = parseXessQuality(next().c_str());
        else if (k == "--width")    opts.outputW = static_cast<UINT>(std::stoul(next()));
        else if (k == "--height")   opts.outputH = static_cast<UINT>(std::stoul(next()));
        else if (k == "--gpu_id")   opts.gpuIndex = static_cast<int>(std::stol(next()));
        else if (k == "--csv")      opts.csvPath = next();
        else if (k == "--sharpness") opts.sharpness = std::stof(next());
        else if (k == "--seconds")  opts.runSeconds = std::stod(next());
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); usage(); return 1; }
    }
    LocalFree(argv);

    if (listAdapters) {
        std::printf("DXGI adapters:\n");
        for (const auto& a : intel::AdapterSelector::enumerate()) {
            std::printf("  vendor 0x%04X device 0x%04X%s | %s\n",
                        a.vendorId, a.deviceId,
                        a.isSoftware ? " (software)" : "",
                        a.name.c_str());
        }
        return 0;
    }

    std::printf("MinovaUpscaler Phase 1 PoC\n");
    std::printf("[xess] libxess.dll version %s\n", XessUpscaler::getVersionString());
    std::printf("mission reminder: this PoC renders *at a reduced internal "
                "resolution* with XeSS-SR reconstruction (true render-"
                "resolution upscaling); it is not a screenshot filter.\n\n");

    PocApp app(opts);
    return app.run();
}
