#pragma once

// Windows.Graphics.Capture window source (D3D11-only).
// Creates a plain D3D11 device (the MS SimpleCapture pattern), hands it to the
// WGC frame pool, and copies each captured frame into a stable texture that
// the FSR presenter samples.
//
// NOTE: on Windows 11 Insider builds 26200.x WGC pool creation currently
// access-violates (machine-level bug); capture_app probes this via the
// wgc_smoke child process and falls back to Desktop Duplication if broken.

#include <d3d11.h>
#include <wrl/client.h>

struct HWND__;
typedef struct HWND__* HWND;

namespace capture {

class WgcSource {
public:
    WgcSource() = default;
    ~WgcSource() { shutdown(); }
    WgcSource(const WgcSource&) = delete;
    WgcSource& operator=(const WgcSource&) = delete;

    /// Uses the presenter's D3D11 device for WGC (one device for everything).
    void initialize(Microsoft::WRL::ComPtr<ID3D11Device> device,
                    HWND targetHwnd);

    /// Returns true when the target's latest frame was copied into the
    /// returned texture. Handles target-window resizes by recreating the pool.
    bool acquireFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>* outTex,
                      unsigned* outW, unsigned* outH);

    void shutdown();

private:
    void recreatePool(unsigned w, unsigned h);

    Microsoft::WRL::ComPtr<ID3D11Device>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;

    // Opaque WinRT state kept here (implementation includes winrt headers).
    void* m_winrt = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_hold;   // stable capture texture
    unsigned m_poolW = 0, m_poolH = 0;
    HWND m_target = nullptr;
};

} // namespace capture
