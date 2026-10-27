#pragma once

// Desktop Duplication capture source (D3D11-only).
// Captures the monitor that contains the target window and copies each new
// desktop frame into a stable texture the FSR presenter samples. The caller
// crops the target's client rect (physical coordinates) out of the frame.
//
// Important constraint: DDI composites what is ON SCREEN, so the captured
// crop must not be occluded - the presenter window is kept away from (or
// minimized while) the target window in this mode.

#include <d3d11.h>
#include <dxgi1_2.h>   // IDXGIOutputDuplication
#include <wrl/client.h>

struct HWND__;
typedef struct HWND__* HWND;
typedef struct tagRECT RECT;

namespace capture {

class DdaSource {
public:
    DdaSource() = default;
    ~DdaSource() { shutdown(); }
    DdaSource(const DdaSource&) = delete;
    DdaSource& operator=(const DdaSource&) = delete;

    void initialize(Microsoft::WRL::ComPtr<ID3D11Device> device,
                    HWND targetHwnd);

    /// True when a fresh desktop frame was captured (LastPresentTime nonzero).
    bool acquireFrame(Microsoft::WRL::ComPtr<ID3D11Texture2D>* outTex,
                      unsigned* outW, unsigned* outH);

    /// Monitor rect (physical screen coordinates) for crop math.
    void monitorRect(int* x, int* y, unsigned* w, unsigned* h) const
    {
        if (x) *x = m_monX;
        if (y) *y = m_monY;
        if (w) *w = m_monW;
        if (h) *h = m_monH;
    }

    void shutdown();

private:
    void recreateDuplication();

    Microsoft::WRL::ComPtr<ID3D11Device>        m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_ctx;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dup;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>     m_hold;   // stable texture

    unsigned m_monW = 0, m_monH = 0;
    int m_monX = 0, m_monY = 0;
    HWND m_target = nullptr;
};

} // namespace capture
