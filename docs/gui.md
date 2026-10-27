# GUI — window picker & settings

`minova_capture.exe` with **no arguments** (or `--gui`) opens the picker:

```
1) เลือกหน้าต่างเกม          <- listbox of visible windows + รีเฟรช (double-click = start)
2) ความละเอียด output        <- 1920x1080 / 1600x900 / 1280x720
3) บังคับเกม render ที่:      <- ไม่บังคับ / 1280x720 / 960x540 / 640x360
4) ความคม (RCAS)             <- slider 0.00–0.60 stops
[ ] Overlay: แสดงบนจอโดยตรงทับเกม (ต้องมี WGC)
[ ] ไม่จำกัด FPS (Present 0) — โหมดเทียบผล
[เริ่ม (Start)] [หยุด (Stop)]              <big fps readout>
บันทึก:                                    <- livechild stdout log
```

## Process isolation

Start spawns the **same exe in CLI mode** as an isolated child
(`CreateProcess` + stdout/stderr piped). The GUI stays alive if the
pipeline crashes; Stop = `TerminateProcess`. The child's `[fps] N` lines
drive the big fps readout (updated every 0.5 s).

## Benchmarking with uncapped FPS

Tick **ไม่จำกัด FPS** — the presenter presents with `Present(0)`:

* what you see is the **pipeline's raw present rate** (GPU headroom);
* with the DDA backend the *content* still refreshes at the monitor's rate;
* the game's own fps is never locked by us — compare the game's built-in
  fps counter / overlay itself.

Recommended flow for A/B results: run once with the checkbox on to record
pipeline headroom, then off for the real play session.

## Troubleshooting

| symptom | cause / fix |
|---|---|
| presenter shows the desktop recursively | DDA captures what is on screen — the app **auto-places (and if needed auto-scales) the presenter in the free strip next to the game**, so this only happens if you later drag the presenter back over the game; move it away |
| "Overlay requested but unavailable" | this OS build's WGC is broken (26200.9457 Unknown) — side window fallback is automatic; update Windows to get overlay |
| black output, target fps 0 | the game was minimized at start — the tool now auto-restores iconic targets; avoid minimizing afterwards (DDA can't see a minimized window) |
| log shows `force-res: client ...` | the game client area was resized to the chosen render size; original size is restored when the session ends |
