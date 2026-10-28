#include "VideoDevices.hpp"

#include <algorithm>
#include <map>

namespace quaddeck {

namespace {

// Calls back with each hardware adapter, its description and which of its
// name it is, until the callback says stop.
template <typename Visit>
void forEachHardwareAdapter(Visit&& visit) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
    std::map<std::wstring, int> seen;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) == S_OK; ++index) {
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        const std::wstring name = description.Description;
        const int ordinal = seen[name]++;
        if (!visit(adapter, description, ordinal)) return;
    }
}

// The monitors' own names ("DELL U2720Q") by the GDI device name of the
// desktop they show; empty where Windows has none.
std::map<std::wstring, std::wstring> monitorNames() {
    std::map<std::wstring, std::wstring> names;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return names;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(),
                           nullptr) != ERROR_SUCCESS) return names;
    for (UINT32 index = 0; index < pathCount; ++index) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[index].sourceInfo.adapterId;
        source.header.id = paths[index].sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = paths[index].targetInfo.adapterId;
        target.header.id = paths[index].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) continue;
        // A mirrored desktop has two monitors; the first one names it.
        names.try_emplace(source.viewGdiDeviceName, target.monitorFriendlyDeviceName);
    }
    return names;
}

}  // namespace

std::vector<VideoAdapterInfo> listVideoAdapters() {
    std::vector<VideoAdapterInfo> list;
    forEachHardwareAdapter([&](const auto&, const DXGI_ADAPTER_DESC1& description, int) {
        list.push_back({description.Description, description.VendorId,
                        static_cast<std::uint64_t>(description.DedicatedVideoMemory)});
        return true;
    });
    return list;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> findVideoAdapter(const std::wstring& name, int ordinal) {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> found;
    if (name.empty()) return found;
    forEachHardwareAdapter([&](const Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter,
                               const DXGI_ADAPTER_DESC1& description, int which) {
        if (name != description.Description || which != ordinal) return true;
        found = adapter;
        return false;
    });
    return found;
}

std::vector<VideoDisplayInfo> listVideoDisplays() {
    std::vector<VideoDisplayInfo> list;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info)) return TRUE;
        VideoDisplayInfo display;
        display.device = info.szDevice;
        display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
        display.left = info.rcMonitor.left;
        display.top = info.rcMonitor.top;
        display.right = info.rcMonitor.right;
        display.bottom = info.rcMonitor.bottom;
        reinterpret_cast<std::vector<VideoDisplayInfo>*>(data)->push_back(std::move(display));
        return TRUE;
    }, reinterpret_cast<LPARAM>(&list));
    const auto names = monitorNames();
    for (auto& display : list) {
        if (const auto name = names.find(display.device); name != names.end()) display.name = name->second;
    }
    // \\.\DISPLAY1 before \\.\DISPLAY2, as Windows' settings number them;
    // EnumDisplayMonitors goes by where they are.
    std::stable_sort(list.begin(), list.end(), [](const VideoDisplayInfo& a, const VideoDisplayInfo& b) {
        if (a.device.size() != b.device.size()) return a.device.size() < b.device.size();
        return a.device < b.device;
    });
    return list;
}

HMONITOR monitorForDisplay(const std::wstring& device) {
    struct Search {
        const std::wstring* device;
        HMONITOR found;
    } search{&device, nullptr};
    if (device.empty()) return nullptr;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto& state = *reinterpret_cast<Search*>(data);
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info) || _wcsicmp(info.szDevice, state.device->c_str()) != 0) return TRUE;
        state.found = monitor;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace quaddeck
