#pragma once

#include <dxgi1_5.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace intel {

/// One enumeration result of an output adapter on this machine.
struct AdapterInfo {
    std::string name;
    std::string description;
    UINT        vendorId = 0;     // e.g. 0x8086 = Intel
    UINT        deviceId  = 0;
    bool        isSoftware = false; // WARP / basic display
};

/// Helpers to enumerate and pick GPUs for the upscaler.
class AdapterSelector {
public:
    /// Enumerate all adapters visible to DXGI.
    static std::vector<AdapterInfo> enumerate();

    /// Pick the adapter the app should run on.
    /// Preference order:
    ///   1. requested GPU index (if valid),
    ///   2. first Intel adapter (vendor 0x8086), non-software,
    ///   3. first hardware adapter of any vendor.
    static Microsoft::WRL::ComPtr<IDXGIAdapter1> pick(int requestedGpuIndex);
};

} // namespace intel

