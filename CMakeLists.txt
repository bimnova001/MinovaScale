cmake_minimum_required(VERSION 3.20)

project(MinovaUpscaler
    LANGUAGES CXX
    DESCRIPTION "Universal Intel iGPU game upscaler (Phase 1 proof of concept)"
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- XeSS SDK (prebuilt, vendored: https://github.com/intel/xess) ---
set(XESS_ROOT     ${CMAKE_SOURCE_DIR}/third_party/xess)
set(XESS_INC_DIR  ${XESS_ROOT}/inc/xess)   # API headers (xess.h, xess_d3d12.h)
set(XESS_LIB_DIR  ${XESS_ROOT}/lib)
set(XESS_BIN_DIR  ${XESS_ROOT}/bin)

# --- AMD FidelityFX FSR 1 reference implementation (vendored, MIT) ---
# We compile ffx_a.h / ffx_fsr1.h against it (CPU-side constants, and by the
# runtime shaders). third_party/ is never modified.
set(FFX_FSR_DIR ${CMAKE_SOURCE_DIR}/third_party/FidelityFX-FSR/ffx-fsr)

add_executable(minova_poc WIN32
    src/main.cpp
    src/app/poc_app.h
    src/app/poc_app.cpp
    src/graphics/d3d12_renderer.h
    src/graphics/d3d12_renderer.cpp
    src/upscaling/iupscaler.h
    src/upscaling/xess_upscaler.h
    src/upscaling/xess_upscaler.cpp
    src/upscaling/fsr1_upscaler.h
    src/upscaling/fsr1_upscaler.cpp
    src/intel/adapter_selector.h
    src/intel/adapter_selector.cpp
    src/telemetry/gpu_clock.h
    src/telemetry/gpu_clock.cpp
)

target_include_directories(minova_poc PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${XESS_INC_DIR}
    ${FFX_FSR_DIR}
)

target_link_libraries(minova_poc PRIVATE
    d3d12 dxgi d3dcompiler
    ${XESS_LIB_DIR}/libxess.lib   # XeSS D3D12 (SR)
)

target_compile_definitions(minova_poc PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
)

if (MSVC)
    target_compile_options(minova_poc PRIVATE /W4 /permissive-)
endif()

# --- Copy XeSS runtime DLLs next to the executable ---
# Per the deployment docs: for D3D12, libxess.dll must sit next to the exe.
# libxell.dll is a runtime dependency of libxess.dll.
foreach(_dll IN ITEMS libxess.dll libxell.dll)
    add_custom_command(TARGET minova_poc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${XESS_BIN_DIR}/${_dll}
            $<TARGET_FILE_DIR:minova_poc>
        VERBATIM)
endforeach()

# Runtime shader compilation reads HLSL next to the exe; the FSR shaders
# #include ffx_a.h / ffx_fsr1.h, so those two headers are copied alongside.
# (Both exes handled by the shared copy loop below.)

source_group("shaders" FILES shaders/scene.hlsl shaders/blit.hlsl
    shaders/fsr_easu.hlsl shaders/fsr_rcas.hlsl)

# Smoke test for the WGC stack (diagnostics only; not shipped).
add_executable(wgc_smoke tools/wgc_smoke.cpp)
target_link_libraries(wgc_smoke PRIVATE d3d11 dxgi windowsapp CoreMessaging)
target_compile_definitions(wgc_smoke PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_executable(dda_smoke tools/dda_smoke.cpp)
target_link_libraries(dda_smoke PRIVATE d3d11 dxgi)
target_compile_definitions(dda_smoke PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

add_executable(pw_smoke tools/pw_smoke.cpp)
target_link_libraries(pw_smoke PRIVATE gdi32 dwmapi)
target_compile_definitions(pw_smoke PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# minova_capture (Phase 2/3): Windows.Graphics.Capture window source +
# FSR 1 presenter. No XeSS dependency (spatial-only reconstruction).
# ---------------------------------------------------------------------------
add_executable(minova_capture WIN32
    src/capture/main.cpp
    src/capture/capture_app.h
    src/capture/capture_app.cpp
    src/capture/wgc_source.h
    src/capture/wgc_source.cpp
    src/capture/dda_source.h
    src/capture/dda_source.cpp
    src/capture/fsr11_presenter.h
    src/capture/fsr11_presenter.cpp
    src/upscaling/fsr1_upscaler.h
    src/upscaling/fsr1_upscaler.cpp
    src/intel/adapter_selector.h
    src/intel/adapter_selector.cpp
)

target_include_directories(minova_capture PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${XESS_INC_DIR}      # fsr1_upscaler.h uses the quality-enum header
    ${FFX_FSR_DIR}
)

target_link_libraries(minova_capture PRIVATE
    d3d11 dxgi d3dcompiler
    windowsapp     # WinRT (Windows.Graphics.Capture)
    dwmapi
)

target_compile_definitions(minova_capture PRIVATE
    NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE
)

if (MSVC)
    target_compile_options(minova_capture PRIVATE /W4 /permissive-)
endif()

# Runtime shaders for the capture exe as well (same FSR shader pair).
foreach(_target IN ITEMS minova_poc minova_capture)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${FFX_FSR_DIR}/ffx_a.h
            ${FFX_FSR_DIR}/ffx_fsr1.h
            $<TARGET_FILE_DIR:${_target}>/shaders
        VERBATIM)
    add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/shaders
            $<TARGET_FILE_DIR:${_target}>/shaders
        VERBATIM)
endforeach()
