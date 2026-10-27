#pragma once

// minova_capture with no arguments opens this GUI: a window picker plus
// settings that spawn the capture pipeline as an isolated child process
// (minova_capture CLI). The GUI survives crashes of the pipeline and shows
// its stdout telemetry ([fps] N lines drive the FPS readout).

namespace gui {

int runGui();

} // namespace gui
