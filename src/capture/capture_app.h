#pragma once

// capture_app: finds the game window, drives the capture session and the FSR
// presenter loop. Capture-based upscaling only: the game renders at its own
// (reduced) resolution, this tool reconstructs that content to a larger
// view. No DLL injection, no game binary modification, no kernel drivers.
//
// Modes:
//   * --overlay (default): the presenter window IS the display target -
//     borderless, click-through, shown over the game's original rect, so the
//     user plays "through" the upscaled view directly on the display.
//     Requires the WGC backend (occlusion-immune window capture).
//   * without --overlay: a normal movable presenter window shows the upscaled
//     frame (used by the DDA fallback).
//   * --force-res W H: resizes the game window so windowed engines render at
//     a lower resolution (window size == render size), then the original
//     rect is covered by the overlay. This is plain Win32 window management,
//     not injection.

#include <string>
#include <vector>

namespace capture {

struct AppOptions {
    std::string windowTitle;   // case-insensitive match (exact preferred)
    unsigned    targetPid      = 0;  // alternative: capture by process id
    unsigned    outW           = 1920;
    unsigned    outH           = 1080;
    int         forceW         = 0;  // 0 = leave the game window size alone
    int         forceH         = 0;
    bool        overlay        = false; // direct-display presenter (needs WGC)
    bool        uncapped       = false; // Present(0): no vsync lock (benchmark)
    float       sharpness      = -1.0f; // <0 = fsr1 default (0.2 stops)
    int         gpuIndex       = -1;    // -1 = automatic (Intel first)
    double      runSeconds     = 0.0;   // 0 = run until the target/quit
};

/// One candidate game window for the GUI picker. hwnd is opaque.
struct WindowEntry {
    unsigned long long hwnd;
    unsigned long      pid;
    std::wstring       title;
};

/// Enumerates visible, non-cloaked top-level windows of OTHER processes.
std::vector<WindowEntry> listCaptureTargets();

int runApp(const AppOptions& opts);

/// --list-windows: prints all visible top-level window titles.
int listWindows();

} // namespace capture
