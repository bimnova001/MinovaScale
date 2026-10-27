#include "adapter_selector.h"

#include <vector>

namespace intel {

namespace {
Microsoft::WRL::ComPtr<IDXGIFactory2> createFactory()
{
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    UINT flags = 0;
#if defined(_DEBUG)
    flags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(factory.GetAddressOf()))))
        return nullptr;
    return factory;
}

std::string toUtf8(const wchar_t* src)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return {};
    std::string out(static_cast<size_t>(len) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, src, -1, out.data(), len, nullptr, nullptr);
    return out;
}
} // namespace

std::vector<AdapterInfo> AdapterSelector::enumerate()
{
    std::vector<AdapterInfo> out;

    auto factory = createFactory();
    if (!factory)
        return out;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> it;
    for (UINT i = 0;
         factory->EnumAdapters1(i, it.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(it->GetDesc1(&desc)))
            continue;

        AdapterInfo info;
        info.name        = toUtf8(desc.Description);
        info.vendorId    = desc.VendorId;
        info.deviceId    = desc.DeviceId;
        info.isSoftware  = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        out.push_back(std::move(info));
    }

    return out;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> AdapterSelector::pick(int requestedGpuIndex)
{
    auto factory = createFactory();
    if (!factory)
        return nullptr;

    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> all;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> it;
    for (UINT i = 0;
         factory->EnumAdapters1(i, it.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        all.push_back(it);
    }

    int count = static_cast<int>(all.size());

    // 1. Explicit user selection (--gpu_id N).
    if (requestedGpuIndex >= 0 && requestedGpuIndex < count)
        return all[static_cast<size_t>(requestedGpuIndex)];

    // 2. Prefer Intel hardware adapter (vendor 0x8086) — the target platform.
    for (auto& a : all) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && d.VendorId == 0x8086 &&
            !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return a;
    }

    // 3. Fall back to the first hardware adapter of any vendor.
    for (auto& a : all) {
        DXGI_ADAPTER_DESC1 d{};
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            return a;
    }

    return nullptr;
}

} // namespace intel
