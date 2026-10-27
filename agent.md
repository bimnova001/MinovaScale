# agent.md — handoff notes for the next agent

Read this first, then `info.md` (the original mission brief), then `docs/phase1_report.md`.
Do not re-do Phase 1 work; verify by building and running, then continue with the next steps section.

## Mission (short version)

`MinovaScale` lets an old laptop's Intel Iris Xe iGPU render games at a lower internal
resolution and reconstruct to 1080p with Intel XeSS-SR (a DLSS-style technique), plus honest
A/B control modes and GPU telemetry. **Rule from `info.md`:** this is *true render-resolution
upscaling* (render at 720p WITH temporal jitter + motion vectors + depth, XeSS reconstructs),
not screenshot enlargement, no DLL injection / anti-cheat bypass, honest labeling everywhere.

## Current status (as of 2026-10-27, after driver update)

* Phase 1 PoC (`minova_poc`) is **complete and verified on real hardware** (i5-1135G7,
  Intel Iris Xe, vendor 0x8086 device 0x9A49, driver **32.0.101.7092**, Windows 11).
* Modes work end-to-end at 60 fps (vsync):
  * `native` — 1080p render baseline (~1.1 ms GPU/frame).
  * `bilinear` — low-res render + linear upsample control (~0.34 ms at 720p scene).
  * `xess` — **XeSS-SR actually runs**: scene 960x540 (balanced) → 1920x1080
    reconstruction, visually verified via window capture + pixel readback; 20 s soak
    (~1080 frames) without crash; ~40–60 ms/frame total in steady state.
* GPU timestamp telemetry CSV works: `minova_poc_telemetry.csv` gets rows every ~60 frames
  (`window_end_frame,mode,avg_gpu_scene_ms,avg_gpu_upscale_ms,avg_gpu_total_ms,cpu_fps`).
* Known rough edge: **occasional early-run `DXGI_ERROR_DEVICE_HUNG (0x887A0006)`** during
  the XeSS JIT burst (~2–3 of 5 runs) — see phase1_report for mitigations (relaunch, AC
  power, TdrDelay=10 admin). Future work: auto-retries on device removed.
* Motion-vector bug (prev matrices never advanced) found & fixed — was causing heavy
  ghosting; before/after captures at repo root (`shot_bilinear.png`, `shot_xess_fixed.png`).
* **Phase 2 added: `--mode fsr`** (AMD FSR 1, EASU+RCAS 2-pass spatial upscale,
  vendored MIT code from `third_party/FidelityFX-FSR`). Verified 4/4 × 20 s stable,
  ~6.5 ms upscale GPU @ 1080p, cycle-through-all-modes safe. See
  `docs/phase2_report.md`. Modes now: `xess` / `bilinear` / `fsr` / `native`
  (`T` cycles xess→bilinear→fsr→native).

## Driver facts (resolved) — keep these in mind

1. Old driver (30.0.101.1404) made `xessD3D12CreateContext` fail with
   `XESS_RESULT_ERROR_UNSUPPORTED_DRIVER` — Intel's own sample failed identically.
   Minimum Intel driver for XeSS on Intel hardware: **31.0.101.4148+** (XeSS 1.0 notes),
   newer is better. Installed now: 32.0.101.7092 — XeSS-SR runs.
2. **Use `D3D_FEATURE_LEVEL_12_1`** when creating the D3D12 device. Forced FL 11_0
   makes the XeSS path hang the GPU in early frames on this hardware.
3. Call `xessD3D12BuildPipelines(context, nullptr, /*blocking*/ true, initFlags)`
   before `xessD3D12Init` (per developer guide) — takes the JIT hit at startup.
4. Debug builds (D3D12 debug layer) remain unusable with XeSS on this machine — test
   in Release. No debug-layer messages print even when the device hangs.
5. Startup flakiness is handled at three layers: registry TDR tolerance
   (**TdrDelay=TdrDdiDelay=10 s**, user must set as admin + reboot; verify with
   `reg query ... /v TdrDelay` → 0xa), and the app's own auto-recovery: on
   `DXGI_ERROR_DEVICE_HUNG` it backs off (2/4/8 s), destroys the renderer, recreates
   device + XeSS context, and continues (see PocApp::run catch block, max 3 attempts).
   Verified: 4/4 25-second xess runs completed, at most one transparent recovery each.
   If BOTH the registry value and recovery are missing, a hang kills the process —
   telling the user to relaunch is the fallback.

## Build & run

Requirements: CMake ≥ 4.x, Visual Studio 2022 (MSVC 17.11+, x64), Windows. No Agility SDK /
dxc needed (runtime `D3DCompileFromFile`, vs_5_0/ps_5_0).

```powershell
cmake -S . -B build            # once
cmake --build build --config Release
# run from the build dir so libxess.dll / libxell.dll / shaders\ are found next to the exe:
.\build\Release\minova_poc.exe --mode xess --seconds 8
```

Build post-steps already copy DTOs: `shaders\*`, `libxess.dll`, `libxell.dll` (see
`CMakeLists.txt`). **Never lose** `third_party\xess\LICENSE.txt` (Intel Simplified
Software License — binary-only redistribution).

CLI options: `--mode xess|bilinear|native` · `--quality ultra_performance|performance|
balanced|quality|ultra_quality|ultra_quality_plus|aa` · `--width W --height H` (output res,
default 1920x1080) · `--gpu_id N` · `--csv PATH` · `--seconds S` (0 = until closed) ·
`--list-adapters` · `--help`. Window keys: `T` cycle mode, `P` pause, `Esc` quit.

## Where things live (map)

```
info.md                       original brief (requirements v. important — section 11)
agent.md                      this file
README.md                     user-facing quickstart
docs/phase1_report.md         verified Phase 1 results on this machine
CMakeLists.txt                build config, DLL/shader copy
src/main.cpp                  CLI parsing
src/app/poc_app.{h,cpp}       Win32 window, message loop, keys; frame try/catch
src/graphics/d3d12_renderer.{h,cpp}  D3D12 device, swapchain (flip-discard), scene
                              MRT pass (HDR R16G16B16A16 + MV R16G16 + D32), blit FSQ
src/upscaling/iupscaler.h     upscaler interface
src/upscaling/xess_upscaler.{h,cpp}  XeSS wrapper (create/init/execute) + quality names
src/intel/adapter_selector.{h,cpp}   DXGI adapter pick (Intel 0x8086 first)
src/telemetry/gpu_clock.{h,cpp}      D3D12 timestamp queries, ring buffer, CSV harvest
shaders/scene.hlsl            rotating cube + checker floor; outputs color/MV
shaders/blit.hlsl             fullscreen pass (samples XeSS output or scene color)
third_party/xess/             vendored official Intel XeSS SDK 2.0.2 (do NOT modify)
  inc/xess/  doc/  bin/  samples/basic_sample_super_resolution_dx12/  LICENSE.txt
```

## Integration conventions already in place (verified against SDK headers + Intel sample)

* Jitter: Halton(2,3), 32-point sequence, applied to the projection of the scene pass as
  `pos.xy += w*(jx*2/W, jy*2/H)`; XeSS execute receives `(jx, -jy)` (offsetY sign flip per
  Intel sample).
* Motion vectors: computed in the MRT shader in **NDC velocity** encoding —
  `initFlags = XESS_INIT_FLAG_USE_NDC_VELOCITY`; provided at low-res; SDK dilates.
* Depth: D32 at low res. With jitter non-zero even before the first `xessD3D12Execute`.
* Per SDK docs, check `xessIsOptimalDriver` after context creation (WARNING, not fatal).
* SRV descriptors must set `Texture2D.MipLevels = 1` explicitly (0 crashed GPU-side).
* `GpuClock` ring: frame F writes slot `(F+1) % kRingFrames`; beginFrame(C) harvests slot
  of frame `C - kHarvestDelay` (fence delay = kFrameCount = 3). Off-by-one already fixed.

## Next steps (in priority order)

1. **Driver-update path**: get driver ≥ 31.0.101.4148 installed on a machine with Iris
   Xe, then verify `--mode xess` truly runs (check init line prints input res, scene at
   720p, telemetry `upscale_ms` nonzero). This is the only remaining untested path from
   the current PoC.
2. Resolution semantics polish: `--width/--height` currently = output res; bilinear's
   scene res is derived by quality factor (same rule as XeSS). Consider a `--scene_*`
   override or documented mapping; keep the honesty rule (XeSS mode must render scene at
   the SDK's input resolution, not screenshot-blit).
3. Phase 2 per `info.md`: move from the synthetic scene toward a real interop/use-case
   (e.g., a mode that games can't inject — our OWN app path, or window-capture-based
   upscaling with clearly honest labeling if we go that route; capture ≠ true upscaling).
4. Telemetry: add CPU frame timing + 1%/0.1% low fps; annotate CSV rows with quality
   setting and scene resolution.
5. Packaging/UX: single-folder "releases", short manual, add `--quality` interplay with
   `xessD3D12Init` input-res printout in the README.
6. Keep licenses intact; track any new third-party additions in README "Third-party".

## House rules for agents that touch this repo

* Verify claims on hardware before documenting them as true. If it can't be run here
  (e.g., XeSS on old driver), say so explicitly in the report, exactly like
  `docs/phase1_report.md` does.
* No DLL injection, no game-hijacking, no anti-cheat circumvention (from `info.md`).
* Debug builds on machines with very old Intel drivers are unreliable due to the D3D12
  debug layer (see above) — treat as environment issue, don't "fix" random device-removed
  errors in Release.
* Don't modify anything under `third_party/xess/` (vendored SDK).
