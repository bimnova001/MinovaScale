# Machine findings — Windows/driver quirks met while building

Environment: i5-1135G7, Iris Xe 0x9A49, driver 32.0.101.7092,
Windows 11 Insider **build 26200.9457 (25H2)**.

| # | finding | status / workaround |
|---|---|---|
| 1 | **WGC pool creation access-violates machine-wide** on this Insider build (`Direct3D11CaptureFramePool::Create/CreateFreeThreaded`, window or monitor, plain D3D11 or D3D11On12). Retry constantly. | `minova_capture` probes WGC in an isolated child (`wgc_smoke.exe`) and falls back to DDA. On normal consumer builds WGC works, so the overlay mode ships. Re-test after OS updates. |
| 2 | **Desktop Duplication works** on this machine — ~60 fps acquisition confirmed | the default fallback backend |
| 3 | **PrintWindow (PW_RENDERFULLCONTENT) hangs forever** on the *2nd* call against a live D3D flip-model window in another process (first call OK, ~39 ms) | `pw_smoke.cpp` keeps the evidence; PW is no longer in the pipeline |
| 4 | `DWMWA_EXTENDED_FRAME_BOUNDS` returns **physical pixels** and is safe for DPI-unaware targets; `GetClientRect` of a DPI-unaware target is virtualized nonsense in-DPI-aware caller (gave 1193×709 for a 960×540 window) | all crop math uses the DWM bounds |
| 5 | **A minimized target breaks capture**: DWM bounds of iconic windows point at (-32000,-32000) → crop 0×0 → black output. | the capture app restores iconic targets at startup; never minimize the game during DDA sessions |
| 6 | **D3D11 flip swapchain** exposes only buffer 0 until buffers rotate (`GetBuffer(>0)` → `DXGI_ERROR_INVALID_CALL`) | fetch the active back buffer fresh each frame |
| 7 | Windows SDK 22621 `IDXGIOutput1::DuplicateOutput` takes **2 arguments** (`device`, out duplication) — nonstandard vs. docs | mirrored the working `tools/dda_smoke.cpp` pattern |
| 8 | XeSS one-time JIT burst can trip `DXGI_ERROR_DEVICE_HUNG`; registry `TdrDelay=10` + PoC auto-recovery (backoff/recreate ≤3 attempts) makes it invisible | PoC handles it; FSR path unaffected |
| 9 | Debug builds with the D3D12 debug layer are unreliable with XeSS here | test PoC in Release; the capture app is fine built either way |
