# Phase 2 report — FSR 1 spatial-upscaling fallback (mode `fsr`)

Date: 2026-10-27 · Same machine as phase 1 (i5-1135G7, Iris Xe 0x9A49,
driver 32.0.101.7092, TdrDelay/TdrDdiDelay = 10 s) · Release build.

## What was added

A fourth upscaling mode `--mode fsr` implementing **AMD FidelityFX Super
Resolution 1.0** as the info.md-mandated spatial fallback:

* scene renders at the same quality-preset-driven internal resolution as the
  XeSS path (scale ratios mirror the documented XeSS presets:
  ultra_performance 3.0× … aa 1.0×; balanced = 960×540 → 1920×1080),
* then two fullscreen passes follow the AMD reference exactly:
  1. **EASU** (edge-adaptive spatial upscaling) → RGBA8 intermediate,
  2. **RCAS** (contrast-adaptive sharpening) → swap-chain back buffer,
* no jitter / motion vectors / depth are used or needed — FSR 1 is spatial;
* RCAS sharpness default = 0.2 stops (`fsr1_upscaler.h:kDefaultSharpnessStops`).

## Files

* `shaders/fsr_easu.hlsl`, `shaders/fsr_rcas.hlsl` — include the vendored
  `ffx_a.h` / `ffx_fsr1.h` (32-bit float path, the AMD sample's
  `SAMPLE_SLOW_FALLBACK` branch — deliberately not the packed-fp16 fast path).
* `src/upscaling/fsr1_upscaler.{h,cpp}` — CPU constant setup
  (`FsrEasuCon`/`FsrRcasCon` via `A_CPU`) + quality→ratio table.
* `src/graphics/d3d12_renderer.cpp` — mode plumbing, EASU/RCAS PSOs
  (shared root sig: SRV table + root CBV + linear-clamp sampler), resources,
  record-time branch with silent fallback to bilinear if any resource is
  missing (never crashes mid-game).
* `CMakeLists.txt` copies the two FFX headers next to the shaders at runtime.

Honesty labeling: `fsr` is announced as spatial-only in `--help`, and the
overall mission text is unchanged — FSR 1 is *not* temporal reconstruction.

## Verified on hardware

| Test (Release) | Result |
|---|---|
| `--mode fsr --seconds 20` × 4 | **4/4 OK**, 60 fps vsync, zero device/hang events |
| Full `T` cycle fsr→bilinear→xess→native→fsr | no crash, app alive after cycle |
| Window capture (`shot_fsr.png`) | cube + checkerboard sharp, RCAS edge contrast visible |
| Telemetry (steady) | scene 0.18 ms · **upscale (EASU+RCAS) ≈ 6.5 ms** · total ≈ 6.7 ms @ 1080p |

Comparison at balanced (960×540 → 1080p) on this iGPU:

| mode | upscale GPU ms | reconstruction |
|---|---|---|
| xess | ~22–30 | temporal (jitter+MV+depth) |
| fsr | ~6.5 | spatial only |
| bilinear | ~0 | (bilinear sampling in blit) |

FSR 1 costs ~4–5× less than XeSS on this iGPU and cannot hang (static PSOs,
no JIT) — the right fallback for stability-critical use; XeSS gives the
higher temporal quality when the driver supports it.

## Notes / limitations

* FSR 1 was designed for LDR sRGB color; our scene color is linear HDR
  (R16G16B16A16). Values here stay 0..1 so the result is well-behaved, but a
  tonemapped/sRGB encode before EASU would follow AMD's guidance more
  strictly — noted as a polish item.
* No per-frame constants updates (scene/output size are fixed per run).
* `--sharpness` CLI knob not yet exposed (default 0.2 stops).
