# Project Brief: Universal Intel iGPU Game Upscaler

## 1. Project mission

Build a Windows application that helps people with older or budget
laptops play games more smoothly without buying a new computer or a
dedicated GPU.

The core target is:

**Render a game at a lower internal resolution (for example, 1280×720)
on an Intel integrated GPU, then reconstruct/upscale the result to a
1920×1080 output.**

The desired user experience is similar in purpose to NVIDIA DLSS: reduce
the GPU cost of rendering while producing a sharper, higher-resolution
final image. This project must not claim to be equivalent to DLSS in
image quality or implementation unless testing proves it.

Initial target hardware: - Intel Iris Xe Graphics integrated GPU - Intel
Core i5-1135G7 laptop - 16 GB system RAM - Windows 10/11 x64

Long-term target: - Public Windows software focused on Intel integrated
GPUs, with possible support for additional Intel GPU generations after
testing.

## 2. User motivation

Many people cannot afford to replace their laptop or buy a new gaming
PC. In many notebooks, the practical upgrades are limited to RAM and
SSD; the GPU cannot be replaced. The project aims to extend the useful
life of these devices by reducing game-rendering cost where technically
possible.

Prioritize: 1. Real performance improvement, not just a higher displayed
FPS number. 2. Good image quality at 1080p output. 3. Low added latency
and low CPU/GPU overhead. 4. Broad compatibility, safe behavior, and
transparent limitations. 5. No requirement for users to modify game
files manually whenever possible.

## 3. The key technical distinction

Do not confuse these two approaches:

### A. True render-resolution upscaling (the actual goal)

The game renders fewer pixels, e.g. 1280×720, and an upscaler
reconstructs an output of 1920×1080. This can reduce GPU rendering
workload.

### B. Upscaling a captured screen (fallback only)

An application captures an already-rendered game image and enlarges it.
This can improve presentation or scaling quality, but **does not reduce
the game's original rendering workload** if the game still rendered at
1080p. It must not be presented as solving the main goal.

The main engineering challenge is making games render at a lower
internal resolution and obtaining the rendering data required for
temporal reconstruction, without modifying every game individually.

## 4. Important limitation: XeSS is not a universal external filter

Intel XeSS Super Resolution (XeSS-SR) is primarily an SDK that game
developers integrate into a game's rendering pipeline. High-quality
temporal reconstruction can require inputs such as: - Rendered color at
the lower resolution - Motion vectors - Depth information - Jitter
information and correct frame integration

A standalone app cannot simply pass an arbitrary screenshot to XeSS-SR
and expect the same result as an in-game integration. Capturing a game's
window alone does not provide all required rendering inputs.

Use the official Intel XeSS SDK as a research and integration option,
not as a claim that XeSS can be enabled in every unsupported game: -
https://github.com/intel/xess -
https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html

A spatial upscaler can work from an image alone, but generally lacks
temporal data and may have different image quality. It is a useful
fallback, not equivalent to temporal reconstruction.

## 5. Recommended technical direction

Start with a proof of concept before building a polished GUI.

### Phase 1 --- Controlled renderer proof of concept

Create a small Direct3D 12 sample application that: 1. Renders a test
scene at 1280×720. 2. Upscales/reconstructs to 1920×1080. 3. Tests the
official XeSS-SR SDK on Intel Iris Xe where supported. 4. Compares
output against native 1080p. 5. Measures GPU frame time, total frame
time, output quality, and latency.

This verifies that the selected SDK/backend and hardware work before
tackling arbitrary games.

### Phase 2 --- Spatial upscaling fallback

Implement a GPU shader-based spatial upscaler (for example, a
well-documented FSR 1-style approach, subject to its license and correct
attribution). Keep the implementation behind a common C++ interface so
the upscaling method can be replaced.

Potential repository: -
https://github.com/GPUOpen-Effects/FidelityFX-FSR

### Phase 3 --- Game compatibility research

Investigate how to request lower rendering resolution and present the
output for different game APIs and display modes. Build a compatibility
matrix for: - DirectX 11 - DirectX 12 - Vulkan - Windowed / borderless /


### Phase 4 --- Public application

After technical feasibility is demonstrated, implement: - Intel GPU
detection and explicit adapter selection - Game/window selection -
Output resolution selection - Upscaling mode and quality settings - FPS
and frame-time measurements - Compatibility logging and safe fallback -
Clear warnings when the app only scales captured output rather than
reducing the game's render resolution

## 6. Proposed architecture

Use C++20 on Windows. Prefer a modular design with Direct3D 11/12
components as required by the chosen implementation.

Suggested modules: - `app/`: lifecycle and application controller -
`capture/`: supported window capture paths (not a substitute for lower
render resolution) - `graphics/`: device, adapter, texture and
presentation management - `upscaling/`: shared `IUpscaler` interface and
implementations - `intel/`: Intel adapter detection and selection -
`telemetry/`: GPU timings, FPS, frame times, latency estimates -
`compatibility/`: per-game test results and limitations - `ui/`:
settings and status UI - `tests/`: correctness, image quality,
performance, and device tests

Do not lock the entire design to one API or one upscaler. Keep render
integration and post-process upscaling as distinct concepts.

## 7. How to measure success

Do not define success as "the image is 1080p" alone.

For every supported test: - Compare native 1080p against 720p render +
1080p upscale. - Measure GPU frame time and full frame time. - Record
actual game-rendered FPS separately from generated/displayed FPS. -
Measure added latency where feasible. - Inspect image quality, ghosting,
flicker, aliasing, and UI/text clarity. - Test stability for at least 30
minutes. - Record hardware, driver, OS, game version, API, and display
mode.

A useful initial success condition is a repeatable reduction in GPU
frame time with acceptable image quality and latency. Set numeric
performance thresholds only after baseline tests.

## 8. Compatibility goal: 90%

The aspirational goal is to support at least 90% of a clearly defined
target set of games. This is **not a guarantee** that 90% of all PC
games will work.

Track two separate metrics: 1. **Capture/presentation compatibility:**
the app can capture or present the game correctly. 2. **True
upscaling/performance success:** the game actually renders at a lower
resolution and the end-to-end result improves performance with
acceptable quality and latency.

Only report the 90% figure after defining a representative game test set
and publishing the results. A screen-capture-only path does not count as
true render-resolution upscaling.

## 9. Licensing and distribution

Before bundling third-party code or SDK binaries: - Read each
repository's current license and redistribution terms. - Preserve
required copyright and attribution notices. - Confirm whether SDK
headers, libraries, runtime components, and sample code may be
redistributed. - Keep third-party code and license notices clearly
separated. - Do not assume a public GitHub repository automatically
permits any form of redistribution.

## 10. Instructions to AI coding assistants

When helping with this project: 1. Keep the mission focused on
**reducing the game's real render resolution and upscaling the output**,
not merely enlarging screenshots. 2. Be explicit about what is proven,
what is a prototype, and what remains research. 3. Do not claim that
XeSS-SR can be enabled universally through an external filter. 4. Do not
invent SDK APIs, build commands, or compatibility claims. Check the
official current documentation/repository when needed. 5. Prefer small,
compilable milestones with exact Windows build and test instructions. 6.
Include complete source files when asked to implement a milestone, plus
dependencies, license notes, and expected output. 7. Test on Intel Iris
Xe / Core i5-1135G7 when possible, and keep hardware-specific
assumptions configurable. 8. Prioritize GPU timing, frame latency,
stability, and image quality over marketing claims. 9. Do not use DLL
injection, anti-cheat bypasses, or unsafe methods as the default
compatibility strategy. Never modify a game's binary or loaded modules;
never bypass or circumvent anti-cheat. What IS allowed as the default
compatibility approach: (a) capture-based upscaling of the game's own
window output, and (b) plain OS/window-level management of games that
have no built-in resolution option - moving or resizing the game window
(SWP/WM style adjustments) or altering its own config/registry/local
file settings, because that does not touch the game binary. Driver-level
(vendor SDK) rendering-resolution forcing remains non-goaled for V1:
the Intel display driver's resolution path is not externally scriptable
in a supported, signed way, so window resize + config edits are the
supported way to lower the render resolution for hard-coded-resolution
games. 10. Clearly label the distinction between a
captured-image upscaler and true game render-resolution upscaling.
11. The user-facing "direct display" mode is a click-through, always-on-
top overlay that covers the game window 1:1 and is fed by a window-capture
backend over FSR 1; it delivers the played-at-1080p experience while the
game itself keeps rendering at the reduced resolution.

## 11. First task for the next AI

Start by reviewing the current official Intel XeSS SDK documentation and
sample projects. Then propose a minimal, buildable Direct3D 12 proof of
concept that renders a controlled scene at 720p and outputs 1080p on
Intel Iris Xe. Identify exact SDK prerequisites and
license/redistribution requirements. Do not start with a universal game
hook or a GUI; establish that the core render-and-upscale path works
first.
