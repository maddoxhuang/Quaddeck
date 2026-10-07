#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <iomanip>
#include <iostream>
#include <iterator>

namespace quaddeck::test {
inline constexpr int missingVideoSupportExitCode = 77;

enum class VideoProbeStage { CreateDevice, VideoDevice, VideoContext };

struct VideoProbeResult {
    VideoProbeStage stage;
    HRESULT result;
};

inline VideoProbeResult queryVideoSupport(ID3D11Device* device, ID3D11DeviceContext* context) {
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice;
    HRESULT result = device->QueryInterface(IID_PPV_ARGS(&videoDevice));
    if (FAILED(result)) return {VideoProbeStage::VideoDevice, result};
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext;
    result = context->QueryInterface(IID_PPV_ARGS(&videoContext));
    return {VideoProbeStage::VideoContext, result};
}

inline VideoProbeResult probeVideoSupport() {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL actual{};
    const HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device, &actual, &context);
    if (FAILED(result)) return {VideoProbeStage::CreateDevice, result};
    return queryVideoSupport(device.Get(), context.Get());
}

constexpr int videoProbeExitCode(VideoProbeResult probe, bool allowMissingVideoSupport) {
    if (SUCCEEDED(probe.result)) return 0;
    // Device creation, driver faults and all later renderer/assertion failures
    // remain failures. Only a missing required video interface can be skipped.
    if (allowMissingVideoSupport && probe.result == E_NOINTERFACE &&
        (probe.stage == VideoProbeStage::VideoDevice ||
         probe.stage == VideoProbeStage::VideoContext))
        return missingVideoSupportExitCode;
    return 1;
}

inline int checkVideoSupport(bool allowMissingVideoSupport) {
    const auto probe = probeVideoSupport();
    const int result = videoProbeExitCode(probe, allowMissingVideoSupport);
    if (result != 0) {
        const char* operation = "D3D11CreateDevice";
        if (probe.stage == VideoProbeStage::VideoDevice) operation = "Query ID3D11VideoDevice";
        if (probe.stage == VideoProbeStage::VideoContext) operation = "Query ID3D11VideoContext";
        std::cerr << (result == missingVideoSupportExitCode ? "SKIPPED: " : "FAILED: ")
                  << operation << " returned 0x" << std::hex
                  << static_cast<unsigned long>(probe.result) << std::dec
                  << "; D3D11 video processing is required for this GPU test.\n";
    }
    return result;
}
}  // namespace quaddeck::test
