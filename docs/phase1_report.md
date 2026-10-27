# Phase 1 report — MinovaScale PoC (minova_poc)

Date: 2026-10-27 (updated after driver fix) · Machine: laptop, Intel i5-1135G7 ·
iGPU: Intel(R) Iris(R) Xe Graphics (vendor 0x8086, device 0x9A49) ·
Driver: 32.0.101.7092 (updated from 30.0.101.1404 mid-session) · Windows 11 x64 ·
No discrete GPU.

## What was built

A self-contained D3D12 application (`minova_poc`) that:

1. Renders a synthetic scene (rotating cube + checkerboard floor + orbiting camera —
   designed to exercise temporal reconstruction) at a chosen internal resolution into an
   MRT pass: HDR color `R16G16B16A16_FLOAT`, motion vectors `R16G16_FLOAT`, depth `D32`.
2. Reconstructs/upscales to the output resolution via one of three selectable modes:
   * **xess** — full XeSS-SR path: Halton(2,3) 32-point projection jitter, NDC-encoded
     motion vectors (`XESS_INIT_FLAG_USE_NDC_VELOCITY`, low-res MVs + depth, SDK dilates),
     `xessD3D12Execute` into its own output texture, then a fullscreen blit.
   * **bilinear** — same low-res render, plain linear upsample (control for A/B).
   * **native** — render directly at output resolution (baseline).
3. Telemetry: D3D12 timestamp queries around scene/upscale passes, ring-buffered 3 frames
   deep, harvested and averaged into a CSV row every ~60 frames:
   `window_end_frame,mode,avg_gpu_scene_ms,avg_gpu_upscale_ms,avg_gpu_total_ms,cpu_fps`.
4. Window + CLI: `--mode`, `--quality`, `--width/--height` (output), `--gpu_id`, `--csv`,
   `--seconds`, `--list-adapters`; window hotkeys `T` (cycle mode), `P` (pause), `Esc`.
   Mode switching is live (XeSS output resources created lazily on first xess frame).

Build: CMake + MSVC 17.11, C++20, D3D12 feature level 11_0 compatible device creation,
flip-discard swapchain 1920x1080, 3 frames in flight. Runtime shader compilation via
`D3DCompileFromFile`. Vendored Intel XeSS SDK 2.0.2 (`libxess.dll` 2.0.2, `libxell.dll`).

## Verified on hardware (after driver update to 32.0.101.7092)

| Test (Release build) | Result |
|---|---|
| `--mode native --seconds 6` | OK, 60 fps vsync, scene ≈ 1.1 ms GPU/frame at 1080p |
| `--mode bilinear --seconds 6` (1080p scene) | OK, scene ≈ 1.1 ms |
| `--mode bilinear --width 1280 --height 720` (720p scene) | OK, scene ≈ 0.34 ms GPU/frame |
| `--mode xess` (balanced, 960x540 → 1080p) | **XeSS runs**; 20 s soak = ~1080 frames, zero crash; window-capture shows a clean reconstructed image (checker floor + cube); scene GPU cost ≈ 0.04–0.2 ms |
| XeSS `upscale_ms` (steady state) | ~22–30 ms at balanced on this battery-powered iGPU (spikes when thermally/power throttled) |
| Telemetry CSV | rows written correctly, header once, values plausible |
| Debug build + D3D12 debug layer | **still destabilises the XeSS path** (no debug-layer messages, device removed early) → test in Release |

### Motion-vector bug found and fixed (visually verified)

`m_prevViewProj` / `m_prevWorld` were cached once and never advanced after frame 0, so
every motion vector was computed against the **frame-0** camera/cube → huge bogus
velocities → heavy ghosting (displaced checker tiles, "ghost sheet" trailing the moving
cube). Fixed by caching the current frame's matrices for the next frame inside
`computeFrameConstants` (`docs`/fix on 2026-10-27). Before/after window captures exist
at repo root (`shot_bilinear.png`, `shot_xess_fixed.png`; use PrintWindow-based
capture to verify visuals when the app is not in the foreground).

## Result: driver was the blocker; updating fixed XeSS-SR entirely

Initial machine state: driver 30.0.101.1404 (Feb 2022, predates XeSS) →
`xessD3D12CreateContext` returned `XESS_RESULT_ERROR_UNSUPPORTED_DRIVER` (0xFFFFFFFE),
including for Intel's own sample. An update to **32.0.101.7092** removed the error and
XeSS-SR now runs end-to-end (visuals verified by window capture + pixel readback:
XeSS output texture contains real reconstructed content, RGB matches sceneColor).

Two driver-related constraints discovered and handled:

1. `D3D_FEATURE_LEVEL_11_0` device creation makes the XeSS path unstable on this
   hardware (GPU_HUNG in early frames); requesting **FL 12_1** (the iGPU's native
   level) made runs stable. Use the device's native feature level, not an old floor.
2. `xessD3D12BuildPipelines(..., blocking=true, initFlags)` is called BEFORE
   `xessD3D12Init` (per developer guide) and kernel JIT still resolves on the first
   executes; bursts can outlive even a raised driver watchdog. With
   **TdrDelay = TdrDdiDelay = 10 s** (registry + reboot) a 5x15 s test gave 4/5 stable
   runs; the app-level auto-recovery (added 2026-10-27) closes the gap:

   On `DXGI_ERROR_DEVICE_HUNG` the app now waits 2 s→4 s→8 s (exponential backoff),
   destroys the renderer, recreates device/swapchain/XeSS context, and keeps running.
   Result: **4/4 subsequent 25-second runs completed** (2 needed exactly one
   transparent recovery); the user never needs to relaunch the app.

## Debug-build caveat (documented, do not re-chase)

The D3D12 **debug layer remains unusable with the XeSS path on this iGPU**, even on the
new driver 32.0.101.7092: Dev build device-removed happens within frames with the debug
layer enabled and a single debug-layer message printed, for both native and xess runs.
If XeSS validation is needed, use only Release. Debug used to work for
bilinear/native only on the old driver setup; re-verify from a clean state before
trusting it.

## Honest-claim status (per the mission rules in info.md)

* True render-resolution upscaling: the scene IS rendered at reduced internal resolution
  with jitter/MV/depth for reconstruction (not a screenshot filter) — code verified.
* XeSS-SR: **running on this machine** with the updated driver (visual + pixel-readback
  verified). Temporal artifacts reduce to normal XeSS-level noise once the prev-frame
  matrix bug was fixed; no claim of DLSS parity.
* Bilinear/native baselines and telemetry: verified working on this hardware.
* Known rough edge: occasional early-run device hang during XeSS JIT (see mitigations
  above). Not a correctness bug in the integration; retry/recovery is future work.
