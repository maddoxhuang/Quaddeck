#include "SoftwareVideoFrame.hpp"
#include "VideoSource.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

using namespace quaddeck;
using Microsoft::WRL::ComPtr;

namespace {
const char* nameOrUnknown(const char* name) { return name ? name : "unknown"; }

bool probe(const wchar_t* path, int width, int height, bool expectHardware,
           ID3D11Device* device, ID3D11DeviceContext* context,
           std::recursive_mutex& deviceMutex) {
    VideoSource source;
    if (!source.open(path, device, context, &deviceMutex,
                     DecodeMode::Automatic, {}, CacheMode::Off)) {
        std::cerr << "Dimensions=" << width << 'x' << height << " open=FAILED\n";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    std::shared_ptr<const HardwareVideoFrame> frame;
    while (std::chrono::steady_clock::now() < deadline) {
        frame = source.frameForTime(0.0, false);
        if (frame || !source.error().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const bool decoderError = !source.error().empty();
    source.close();
    if (!frame) {
        std::cerr << "Dimensions=" << width << 'x' << height
                  << " first-frame=" << (decoderError ? "DECODE-FAILED" : "TIMEOUT") << '\n';
        return false;
    }
    // Examine the retained frame after the producer and codec have closed.
    // No pixel readback, renderer, window or audio sink is created by this probe.
    const AVFrame* owner = frame->ownerFrame;
    if (!owner) {
        std::cerr << "Dimensions=" << width << 'x' << height << " retained-owner=FAILED\n";
        return false;
    }
    auto pixels = static_cast<AVPixelFormat>(owner->format);
    if (frame->hardware && owner->hw_frames_ctx) {
        const auto* frames = reinterpret_cast<const AVHWFramesContext*>(owner->hw_frames_ctx->data);
        pixels = frames->sw_format;
    }
    const auto* descriptor = av_pix_fmt_desc_get(pixels);
    const int depth = descriptor && descriptor->nb_components ? descriptor->comp[0].depth : 0;
    const bool metadata = owner->colorspace == AVCOL_SPC_BT709 &&
        owner->color_primaries == AVCOL_PRI_BT709 && owner->color_trc == AVCOL_TRC_BT709 &&
        owner->color_range == AVCOL_RANGE_MPEG;
    const bool originalSize = owner->width == width && owner->height == height;
    const bool cpuBridge = !frame->hardware && softwareNv12Eligible(*owner);
    const bool route = frame->hardware == expectHardware &&
        (expectHardware ? (owner->format == AV_PIX_FMT_D3D11 && frame->texture != nullptr)
                        : (cpuBridge && frame->texture == nullptr && !frame->bgra.empty()));
    const bool passed = !decoderError && originalSize && metadata && depth == 8 && route;
    std::cout << "Dimensions=" << width << 'x' << height
              << " ExpectedDecode=" << (expectHardware ? "D3D11VA" : "CPU")
              << " ActualDecode=" << (frame->hardware ? "D3D11VA" : "CPU")
              << " Format=" << nameOrUnknown(av_get_pix_fmt_name(static_cast<AVPixelFormat>(owner->format)))
              << " PixelFormat=" << nameOrUnknown(av_get_pix_fmt_name(pixels))
              << " Depth=" << depth
              << " OriginalSize=" << owner->width << 'x' << owner->height
              << " Matrix=" << nameOrUnknown(av_color_space_name(owner->colorspace))
              << " Transfer=" << nameOrUnknown(av_color_transfer_name(owner->color_trc))
              << " Primaries=" << nameOrUnknown(av_color_primaries_name(owner->color_primaries))
              << " Range=" << nameOrUnknown(av_color_range_name(owner->color_range))
              << " CpuNv12Eligible=" << cpuBridge
              << " RetainedOwner=1 Result=" << (passed ? "PASS" : "FAIL") << '\n';
    return passed;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "Usage: QuadDeckHdrDecodeProbe <4320x2160-synthetic.mp4> <1920x1080-synthetic.mp4>\n";
        return 2;
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const std::array<D3D_FEATURE_LEVEL, 2> levels{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
        levels.data(), static_cast<UINT>(levels.size()), D3D11_SDK_VERSION,
        &device, &level, &context);
    std::cout << "D3D11CreateDevice HRESULT=0x" << std::hex << std::setw(8)
              << std::setfill('0') << static_cast<unsigned long>(hr) << std::dec << '\n';
    if (FAILED(hr)) return 3;
    std::recursive_mutex deviceMutex;
    const bool wide = probe(argv[1], 4320, 2160, false, device.Get(), context.Get(), deviceMutex);
    const bool standard = probe(argv[2], 1920, 1080, true, device.Get(), context.Get(), deviceMutex);
    return wide && standard ? 0 : 1;
}
