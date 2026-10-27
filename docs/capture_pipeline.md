# Capture-based upscaler — architecture (Phase 3)

The user-facing goal: a game without resolution options (e.g. Roblox) is
shrunk to 720p-class by **plain window management**, then this tool captures
that window and reconstructs it to 1080p-class with **AMD FSR 1 (EASU+RCAS)**.
No DLL injection, no kernel drivers, no game binary touched — see
[rules](rules.md).

<p align="center"><img src="pipeline.svg" width="820" alt="pipeline"></p>

## One device, D3D11 only

`fsr11_presenter` (`src/capture/fsr11_presenter.cpp`) creates **one D3D11
device** that is used for *everything*: the swapchain, the WGC frame pool and
the Desktop-Duplication output. The immediate context serializes
capture copy → EASU → RCAS → Present, so there are no shared handles,
no fence rings and no D3D12 barrier states anywhere. The earlier D3D12
design (11on12 queue sharing) was dropped for exactly this reason.

Pipeline per frame (fullscreen triangle, no vertex buffers):

```
capture texture (BGRA) --EASU--> intermediate (BGRA) --RCAS--> swapchain backbuffer
```

* Constants come from `fsr1::computeConstantsCrop` — vendored
  `FsrEasuConOffset` (input viewport = cropped client area, input image =
  full capture texture).
* The swapchain is `FLIP_DISCARD`, 3 buffers, `B8G8R8A8_UNORM`. Note: a
  D3D11 flip swapchain only exposes buffer 0 until buffers rotate, so the
  active back buffer is fetched fresh each frame.

## Capture backends

| backend | file | notes |
|---|---|---|
| WGC (Windows.Graphics.Capture) | `src/capture/wgc_source.cpp` | captures window content even when occluded → required for overlay mode; target-window resizes handled via `pool.Recreate` |
| DDA (IDXGIOutputDuplication) | `src/capture/dda_source.cpp` | monitor-rectangle capture; healthy-OS fallback; **recursion caveat**: the presenter window itself must not overlap the game's screen area |
| PrintWindow | `tools/pw_smoke.cpp` only | **removed from the pipeline** — hangs on the 2nd call against live D3D flip windows (evidence in `tools/pw_smoke.cpp`) |

WGC health is probed in an **isolated child process** (`wgc_smoke.exe`,
spawned with `probeWgcHealthy()`); on Windows 11 Insider build 26200.9457
WGC crashes machine-wide, so the tool gracefully falls back to DDA there —
see [machine findings](machine_findings.md).

## Cropping

* **WGC:** the frame contains the whole window *including borders*. The
  client offset is scaled into frame pixels via `DWMWA_EXTENDED_FRAME_BOUNDS`
  (`queryClientCrop`).
* **DDA:** the frame is the whole monitor. The crop is the target's DWM
  extended-frame bounds clipped to the monitor (`queryDesktopCrop`) — these
  are *physical pixels*, safe even for DPI-unaware targets where
  `GetClientRect` is virtualized.
* On startup the tool **restores a minimized target** (iconic windows report
  off-screen bounds ≈ (-32000,-32000) → a zero-size crop → black output;
  that exact bug cost a whole debug session).

## Forcing the render resolution (`--force-res W H`)

`SetWindowPos` with an outer size derived from `AdjustWindowRectEx` for the
game's current style, so the **client area** becomes exactly W×H. This is
ordinary window management — the honest allowed lever for games with no
built-in resolution setting.

## Direct-display mode (`--overlay`)

* Presenter window = `WS_POPUP` + `WS_EX_TOPMOST | WS_EX_LAYERED |
  WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` (`WS_EX_TOOLWINDOW`).
* `SetLayeredWindowAttributes(255)` keeps it fully opaque while the
  `WS_EX_TRANSPARENT` makes mouse input pass through; the game keeps focus.
* Sized/positioned **1:1 over the game's original rect** (captured before
  the force-res), shown with `SW_SHOWNOACTIVATE`.
* Requires WGC (captures through occlusion). Without WGC the tool warns and
  uses a plain side window instead — full-screen overlay presentation can't
  be validated on this machine until the OS WGC bug is fixed.

## Uncapped FPS (`--novsync`)

`Present(0)` instead of `Present(1,0)`. Two honest caveats:

1. **Present-loop fps** becomes uncapped (often >1000 fps on trivial content)
   — that measures *GPU headroom of the presenter pipeline*, not game fps.
2. **Acquisition** is still capped by the backend: DDA delivers new frames at
   the monitor's refresh rate; the loop re-presents the last captured
   texture between new frames.

`[fps] N` is also printed to stdout every 0.5 s — the GUI parses it.

## Verification log

* Quadrant debug shader (`MINOVA_DEBUG_SHADER=1`): Q1 nearest@crop,
  Q2 linear@crop, Q3 load-whole-texture, Q4 raster-solid — used to isolate
  the minimized-window crop bug (all four quadrants correct after the fix).
* Soak test 30 s @ forced 800×450 → 1280×720: `fps 60` stable, exit 0.
* Desktop D3D12 texture content verified pre-rewrite via one-shot readback;
  EASU constants verified against the PoC renderer (same shaders, Phase 2).

## CLI reference

```
--window NAME      visible window title substring to capture
--pid N            capture by process id (used by the GUI)
--out W H          presenter size (default 1920x1080)
--force-res W H    shrink the game's client area to WxH
--overlay          direct-display click-through mode (needs WGC)
--novsync          Present(0), no 60 Hz lock (benchmarking)
--sharpness X      RCAS stops, 0 = max sharpen (default 0.2)
--seconds S        auto-quit after S seconds
--gpu_id N         DXGI adapter index (default: first Intel adapter)
--list-windows     print visible window titles
--gui              GUI picker (default when run with no arguments)
```
