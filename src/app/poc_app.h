#pragma once

#include "graphics/d3d12_renderer.h"
#include "upscaling/xess_upscaler.h"

#include <cstdint>
#include <string>

struct HWND__;
typedef struct HWND__* HWND;

/// Options parsed from the command line (see main.cpp).
struct PocOptions {
    UINT                    outputW   = 1920;
    UINT                    outputH   = 1080;
    UpscaleMode             mode      = UpscaleMode::XeSS;
    xess_quality_settings_t quality   = XESS_QUALITY_SETTING_BALANCED;
    int                     gpuIndex  = -1;   // -1 = automatic (Intel first)
    std::string             csvPath   = "minova_poc_telemetry.csv";
    double                  runSeconds = 0.0; // 0 = run until the window closes
    float                   sharpness = -1.0f; // RCAS stops; <0 = default 0.2
};

/// Win32 window + main loop controller around D3D12Renderer.
///
/// Key bindings:
///   T  — cycles upscale mode (xess -> bilinear -> native)
///   P  — pause/resume camera animation
///   Esc — quit
class PocApp {
public:
    PocApp(const PocOptions& options);
    ~PocApp();

    /// Runs the show until the window closes or runSeconds elapses.
    /// Returns process exit code (0 = clean).
    int run();

    static LRESULT CALLBACK wndProcThunk(HWND, UINT, WPARAM, LPARAM);

private:
    LRESULT wndProc(HWND, UINT, WPARAM, LPARAM);
    void updateTitle();

    PocOptions  m_opts;
    HWND        m_hwnd = nullptr;
    D3D12Renderer m_renderer;
    bool        m_running = false;
    unsigned    m_retries = 0;   // device-removed auto-recovery attempts
};
