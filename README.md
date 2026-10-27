# MinovaScale — DLSS-style upscaling PoC for Intel Iris Xe laptops (Phase 1)

Lowers the internal render resolution (e.g. 720p) and reconstructs to 1080p with
**Intel XeSS-SR**, with honest A/B modes (`native`, `bilinear`, `xess`) and GPU timing
telemetry. True render-resolution upscaling — the scene really renders at the lower
internal resolution with temporal jitter + motion vectors + depth. No DLL injection, no
anti-cheat bypass, no screenshot filtering.

See `agent.md` (continuation notes for other agents), `docs/phase1_report.md` (verified
results), `info.md` (original mission).

## Build (Windows, Visual Studio 2022, x64)

```powershell
cmake -S . -B build
cmake --build build --config Release
.\build\Release\minova_poc.exe --mode xess --seconds 8
```

Run from the build directory (`build\Release\` or `build\Debug\`) — `libxess.dll`,
`libxell.dll` and `shaders\` are copied next to the exe by the build.

## Usage

```
minova_poc.exe [--mode xess|bilinear|native]
               [--quality ultra_performance|performance|balanced|quality|
                          ultra_quality|ultra_quality_plus|aa]
               [--width W --height H]      # output resolution (default 1920x1080)
               [--gpu_id N] [--csv PATH] [--seconds S] [--list-adapters] [--help]
```

Window hotkeys: `T` cycle mode · `P` pause · `Esc` quit.

## Status on this machine (driver 32.0.101.7092)

* `native`, `bilinear`, telemetry CSV, window/keys — **verified working** (60 fps vsync).
* `xess` — **runs end-to-end**: scene rendered at 960x540 (balanced), XeSS reconstructs
  to 1920x1080; verified visually and by telemetry (see `docs/phase1_report.md`).
* `fsr` — **new in Phase 2**: AMD FSR 1 (EASU+RCAS) spatial upscale from the same
  quality-preset internal resolution. 4/4 × 20 s stable, ~6.5 ms upscale GPU @ 1080p,
  no JIT/hang risk. See `docs/phase2_report.md`. Press `T` to cycle
  xess → bilinear → fsr → native.
* Known rough edge (handled): rare early-run `DXGI_ERROR_DEVICE_HUNG` during the
  one-time XeSS JIT burst. The app now **auto-recovers** (backoff 2/4/8 s, recreate
  device + XeSS context, max 3 attempts) — verified 4/4 completed 25 s runs. Together
  with registry TdrDelay=10 s (admin + reboot), this is effectively invisible.
* Debug build (with the D3D12 debug layer) is unreliable with XeSS on this machine —
  test in Release.

## Third-party / licenses

* Intel XeSS SDK 2.0.2 — vendored, unmodified, under `third_party/xess/`
  (Intel Simplified Software License: **binary-only redistribution** — keep
  `third_party/xess/LICENSE.txt` when shipping).
