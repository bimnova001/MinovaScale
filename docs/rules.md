# Hard rules (what we may and may not touch)

From the project brief (`info.md`, section 10) plus the decisions made mid-project.

## Not allowed — always

* **No DLL injection** into any game, and **no modification of a game's
  binary** or its loaded modules.
* **No anti-cheat bypass or circumvention** (EAC/BattlEye & co. must see an
  untampered process; hooking/injecting triggers exactly the path we refuse
  to take).
* No kernel drivers, no driver-signing workarounds.

## Allowed — the supported levers

1. **Capture-based upscaling** — WGC / Desktop Duplication capture of the
   game's own window output, reconstructed by FSR 1 / XeSS outside the game.
   The game keeps rendering its own frames; we only read pixels.
2. **OS/window-level management** — for games with no built-in resolution
   option: move/resize the window (`SetWindowPos`), change its desktop
   shortcut/config-file/local-storage settings. This does not touch the
   binary.
3. **The overlay window** is ours: borderless, click-through, topmost,
   covering the game's original rect so the player experiences a fullscreen-
   1080p game while the game really renders 720p-class underneath.

## Why not driver-level render-resolution forcing (V1 decision)

Vendor display drivers (Intel here) don't expose a supported, signed way to
force per-app render resolution from outside. Without an EV-certified
WHQL-signed driver the remaining options are exactly the injection/kernel
territory we exclude. Window resize + config edits achieve the same result
(windowed engines render at the window's client size) through safe,
supported APIs.

## Honesty requirements

* Label capture latency honestly: ≈1–2 frames.
* FSR 1 is **spatial** — on animated scenes its quality is below a temporal
  upscaler (XeSS); say so, don't market it as DLSS-class.
* Never claim "we made the GPU render lower" when only the presentation was
  scaled — the whole architecture exists to keep that distinction true.
