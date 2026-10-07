#include "D3DRenderer.hpp"
#include "ColorConversion.hpp"
#include "Diagnostics.hpp"
#include "ShaderCompat.hpp"
#include "SoftwareVideoFrame.hpp"

#include <d3dcompiler.h>
#include <dxgi1_3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace quaddeck {

namespace {
constexpr char kVertexShader[] = R"(
cbuffer UvTransform : register(b0) { float4 uvTransform; };
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float2 uv2 : TEXCOORD2;
    float2 uv3 : TEXCOORD3;
};
VSOut main(uint id : SV_VertexID) {
    const float2 positions[4] = {
        float2(-1.0,  1.0), float2( 1.0,  1.0),
        float2(-1.0, -1.0), float2( 1.0, -1.0)
    };
    const float2 coords[4] = {
        float2(0.0, 0.0), float2(1.0, 0.0),
        float2(0.0, 1.0), float2(1.0, 1.0)
    };
    VSOut output;
    output.position = float4(positions[id], 0.0, 1.0);
    output.uv0 = coords[id] * uvTransform.xy + uvTransform.zw;
    // PotPlayer/MPC shaders found in the wild do not consistently use the
    // same TEXCOORD index. Supply the transformed video coordinate on the
    // common legacy indices so an absent semantic cannot collapse sampling
    // to the top-left pixel and turn a pane into one solid colour.
    output.uv1 = output.uv0;
    output.uv2 = output.uv0;
    output.uv3 = output.uv0;
    return output;
})";

constexpr char kPixelShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
})";

// Software fallback remains SDR while the selected effect runs. This final
// presentation pass maps its G22 / BT.709 RGB to absolute PQ / BT.2020. Merely
// writing the original 8-bit codes into a PQ swap chain makes SDR white 10,000
// nits; fallback must be correct even when the driver refuses the video path.
constexpr char kSdrToPqShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
cbuffer SdrOutputParams : register(b0) { float4 outputParams; };
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    float3 linear709 = pow(saturate(color.rgb), 2.2);
    float3 linear2020 = float3(
        dot(linear709, float3(0.6274040, 0.3292820, 0.0433136)),
        dot(linear709, float3(0.0690970, 0.9195400, 0.0113612)),
        dot(linear709, float3(0.0163916, 0.0880132, 0.8955950)));
    float3 p = pow(saturate(linear2020 * outputParams.x / 10000.0), 2610.0 / 16384.0);
    float3 pq = pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
                   (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
    return float4(pq, color.a);
})";

constexpr char kSharpenShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    uint width, height; videoTexture.GetDimensions(width, height);
    float2 pixel = 1.0 / float2(width, height);
    float4 center = videoTexture.Sample(videoSampler, uv) * 5.0;
    center -= videoTexture.Sample(videoSampler, uv + float2(pixel.x, 0));
    center -= videoTexture.Sample(videoSampler, uv - float2(pixel.x, 0));
    center -= videoTexture.Sample(videoSampler, uv + float2(0, pixel.y));
    center -= videoTexture.Sample(videoSampler, uv - float2(0, pixel.y));
    return float4(saturate(center.rgb), 1.0);
})";

constexpr char kGrayscaleShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    float gray = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    return float4(gray, gray, gray, color.a);
})";

constexpr char kInvertShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    return float4(1.0 - color.rgb, color.a);
})";

// Native D3D11 form of Smart Vibrance Plus. The SDR branch is a rewrite of
// aston89's Smart_Vibrance_Plus.fx, GPL-3.0
// (https://github.com/aston89/Smart-vibrance-for-reshade), which is why
// QuadDeck as a whole is GPL-3.0. The reference ReShade effect is a
// single stateless pass; its four user uniforms live in b1 so the existing b0
// PotPlayer compatibility contract remains unchanged. b2 describes this pane's
// converted texture: SDR, RTX-produced PQ, or PQ to leave untouched.
constexpr char kSmartVibrancePlusShader[] = R"(
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);
cbuffer SmartVibranceParams : register(b1) {
    float intensity;
    float saturationPivot;
    float grayPivot;
    float graySharpness;
};
cbuffer VibranceColorSpace : register(b2) { float4 colorSpace; };

float sigmoid(float x) {
    return 1.0 / (1.0 + exp(-x));
}

float3 smartVibrance(float3 color) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    float3 chroma = color - luminance;
    float chromaEnergy = dot(chroma, chroma);
    float chromaMagnitude = sqrt(chromaEnergy);
    float normalizedSaturation = saturate(chromaMagnitude / saturationPivot);
    float rolloff = 1.0 - normalizedSaturation;
    float grayProtection = sigmoid((grayPivot - chromaEnergy) * graySharpness);
    float response = lerp(rolloff, 1.0, grayProtection);
    float gain = (intensity - 1.0) * response;
    return saturate(luminance + chroma + chroma * gain);
}

// ST 2084, absolute linear RGB in nits. The SDR preset above stays unchanged.
float3 pqToNits(float3 code) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(code), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / max(c2 - c3 * p, 1e-6), 1.0 / m1);
}

float3 nitsToPq(float3 nits) {
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    float3 p = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 hdrVibrance(float3 code) {
    // Avoid even PQ round-trip error at the neutral setting.
    if (intensity == 1.0) return code;
    float3 rgb = pqToNits(code);
    float y = dot(rgb, float3(0.2627, 0.6780, 0.0593));
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    float low = min(rgb.r, min(rgb.g, rgb.b));
    if (y <= 0.01 || y >= 1000.0 || peak - low <= peak * 1e-5) return code;

    float3 chroma = rgb - y;
    // Relative colourfulness, independent of exposure. Gray controls suppress
    // near-neutral noise here; their SDR reference semantics remain above.
    float3 relativeChroma = chroma / max(peak, 0.01);
    float energy = dot(relativeChroma, relativeChroma);
    float grayMask = smoothstep(0.0, grayPivot, energy) *
                     sigmoid((energy - grayPivot) * graySharpness);
    float saturation = (peak - low) / peak;
    float rolloff = 1.0 - smoothstep(saturationPivot * 0.75, 1.0, saturation);
    // Conservative artistic defaults, not display calibration or tone mapping.
    float exposure = smoothstep(0.01, 1.0, y) * (1.0 - smoothstep(203.0, 1000.0, y));
    // Reduce enhancement in the warm red-yellow region, including many skin
    // tones. This is a hue heuristic, not a face/skin detector.
    float warm = smoothstep(0.0, 0.25, (rgb.r - rgb.g) / peak) *
                 smoothstep(0.0, 0.25, (rgb.g - rgb.b) / peak);
    float extra = (intensity - 1.0) * grayMask * rolloff * exposure * (1.0 - 0.5 * warm);
    if (abs(extra) < 1e-7) return code;

    // Compress the requested gain smoothly toward the legal BT.2020/PQ RGB
    // boundary along the constant-Y gray axis. Per-channel clipping would
    // change Y and hue. This does not model a particular display's gamut.
    float3 room = float3(10000.0 - y, 10000.0 - y, 10000.0 - y);
    room = float3(chroma.r < 0.0 ? y : room.r,
                  chroma.g < 0.0 ? y : room.g,
                  chroma.b < 0.0 ? y : room.b);
    float3 limits = room / max(abs(chroma), 1e-6);
    float headroom = max(0.0, min(limits.r, min(limits.g, limits.b)) - 1.0);
    if (extra > 0.0) extra *= headroom / max(headroom + extra, 1e-6);
    return nitsToPq(y + chroma * (1.0 + extra));
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 color = videoTexture.Sample(videoSampler, uv);
    if (colorSpace.x > 1.5) return color;
    return float4(colorSpace.x > 0.5 ? hdrVibrance(color.rgb) : smartVibrance(color.rgb), color.a);
}
)";

// Blends the interface layer over the video in the 10-bit PQ output mode.
// The layer is premultiplied sRGB as Direct2D left it; each pixel is
// un-premultiplied, linearised, scaled to the display's SDR white level and
// PQ-encoded, then premultiplied again for the ONE / INV_SRC_ALPHA blend.
// Blending in PQ rather than linear light is a small error on a translucent
// pill, and what every SDR-over-HDR composition on Windows accepts.
constexpr char kOverlayCompositeShader[] = R"(
Texture2D overlayTexture : register(t0);
SamplerState overlaySampler : register(s0);
cbuffer OverlayParams : register(b0) { float4 overlayParams; };

float3 srgbToLinear(float3 c) {
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

float3 linearToPq(float3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 y = pow(saturate(nits / 10000.0), m1);
    return pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 layer = overlayTexture.Sample(overlaySampler, uv);
    if (layer.a <= 0.0) return float4(0.0, 0.0, 0.0, 0.0);
    float3 straight = saturate(layer.rgb / layer.a);
    float3 pq = linearToPq(srgbToLinear(straight) * overlayParams.x);
    return float4(pq * layer.a, layer.a);
})";

// NVIDIA's RTX Video features are private D3D11 video-processor stream
// extensions. The GUIDs and the version/method words are the ones mpv
// (vf_d3d11vpp.c) and MPC Video Renderer (D3D11VP.cpp) ship, checked
// against each other; NVIDIA publishes no header. Super Resolution and
// RTX Video HDR ("TrueHDR") are two different interfaces.
constexpr GUID kNvidiaSuperResolutionInterface = {
    0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
constexpr GUID kNvidiaTrueHdrInterface = {
    0xfdd62bb4, 0x620b, 0x4fd7, {0x9a, 0xb3, 0x1e, 0x59, 0xd0, 0xd5, 0x44, 0xb3}};
struct NvidiaSuperResolutionExtension {
    UINT version{1};
    UINT method{2};
    UINT enable{};
};
struct NvidiaTrueHdrExtension {
    UINT version{4};
    UINT method{3};
    UINT enable : 1;
    UINT reserved : 31;
};
constexpr UINT kNvidiaVendorId = 0x10DE;

std::string narrow(const std::wstring& text) {
    std::string result;
    result.reserve(text.size());
    for (const wchar_t c : text) result.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return result;
}

}

namespace {
bool lostDeviceResult(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == DXGI_ERROR_DEVICE_HUNG || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
}  // namespace

D3DRenderer::~D3DRenderer() { releaseDeviceResources(); }

void D3DRenderer::releaseDeviceResources() {
    // The swap chain hands out a handle the caller owns.
    if (frameLatencyWaitable_) {
        CloseHandle(frameLatencyWaitable_);
        frameLatencyWaitable_ = nullptr;
    }
    slots_ = {};
    softwareUploads_ = {};
    softwarePq_ = {};
    loggedSoftwareRoute_.fill(-1);
    renderTarget_.Reset();
    overlayLayerView_.Reset();
    overlayLayer_.Reset();
    overlayCompositeShader_.Reset();
    overlayBlend_.Reset();
    overlayParamsBuffer_.Reset();
    sampler_.Reset();
    pixelShader_.Reset();
    copyShader_.Reset();
    sdrToPqShader_.Reset();
    sdrToPqParams_.Reset();
    smartVibranceBuffer_.Reset();
    vibranceColorBuffer_.Reset();
    smartVibranceSelected_ = false;
    pixelParamsBuffer_.Reset();
    uvTransformBuffer_.Reset();
    vertexShader_.Reset();
    swapChain_.Reset();
    videoContext1_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    if (context_) context_->ClearState();
    frameQuery_.Reset();
    context_.Reset();
    device_.Reset();
    adapter_.Reset();
    swapChainFlags_ = 0;
    hdrOutput_ = false;
    enhancementStatus_.hdrOutput = false;
    enhancementStatus_.superResolutionApplied = false;
    enhancementStatus_.rtxHdrApplied = false;
    loggedSuperResolution_ = -1;
    loggedRtxHdr_ = -1;
}

bool D3DRenderer::recover(HWND window) {
    std::scoped_lock lock(deviceMutex_);
    releaseDeviceResources();
    // Keep the latch raised throughout reconstruction. If any stage fails,
    // App must see deviceLost() again and make its bounded retry rather than
    // treating an empty renderer as healthy forever.
    deviceLost_ = true;
    deviceRebuildInProgress_ = true;
    error_.clear();
    // smartVibranceSettings_ survives the rebuild and seeds the new constant
    // buffer; the caller restores the shader selection, which it owns.
    const bool recovered = initialize(window);
    deviceRebuildInProgress_ = false;
    deviceLost_ = !recovered;
    return recovered;
}

void D3DRenderer::noteFailure(const char* operation, HRESULT hr) {
    setError(operation, hr);
    if (!lostDeviceResult(hr)) return;
    deviceLost_ = true;
    HRESULT reason = hr;
    if (device_) reason = device_->GetDeviceRemovedReason();
    std::ostringstream out;
    out << "GPU DEVICE LOST during " << operation << ": removed reason 0x" << std::hex
        << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(reason);
    appendDiagnostic(out.str());
}

bool D3DRenderer::initialize(HWND window) {
    window_ = window;
    RECT client{};
    GetClientRect(window, &client);
    width_ = std::max(1L, client.right - client.left);
    height_ = std::max(1L, client.bottom - client.top);
    if (!(createDevice(window) && createPipeline() && createBackBuffer())) return false;
    // The swap chain is born 8-bit; a PQ request is a mode switch on top of
    // a working renderer, and its failure leaves the 8-bit chain in place.
    applyOutputMode();
    return true;
}

bool D3DRenderer::createDevice(HWND window) {
    constexpr D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL actual{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
        &device_, &actual, &context_);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &device_, &actual, &context_);
    }
    if (FAILED(hr)) {
        setError("D3D11CreateDevice", hr);
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) { setError("Query IDXGIDevice", hr); return false; }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { setError("GetAdapter", hr); return false; }
    adapter_ = adapter;
    {
        // D3D11CreateDevice with a null adapter takes the one driving the
        // primary display. On a machine with an integrated GPU as well, that
        // decides whether NVIDIA's extensions can exist at all, so say which.
        DXGI_ADAPTER_DESC adapterDescription{};
        if (SUCCEEDED(adapter->GetDesc(&adapterDescription))) {
            enhancementStatus_.adapterName = adapterDescription.Description;
            enhancementStatus_.nvidiaAdapter = adapterDescription.VendorId == kNvidiaVendorId;
            std::ostringstream out;
            out << "GPU adapter: " << narrow(enhancementStatus_.adapterName) << " (vendor 0x"
                << std::hex << adapterDescription.VendorId << ", device 0x"
                << adapterDescription.DeviceId << ")";
            appendDiagnostic(out.str());
        }
    }
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { setError("Query IDXGIFactory2", hr); return false; }

    // A frame-latency waitable swap chain is what makes vsync affordable here.
    // Present must stay inside the same critical section as the D3D11VA
    // decoders, so a Present that blocked for the vertical blank would stall
    // every decoder for most of a refresh interval. Waiting on the swap
    // chain's own event instead happens outside that lock, and by the time the
    // renderer presents, the queue has room and Present returns promptly.
    DXGI_SWAP_CHAIN_DESC1 description{};
    description.Width = width_;
    description.Height = height_;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.BufferCount = 2;
    description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    description.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    swapChainFlags_ = description.Flags;
    hr = factory->CreateSwapChainForHwnd(
        device_.Get(), window, &description, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) {
        // Waitable swap chains need Windows 8.1 or newer. Fall back to a plain
        // flip chain; presentation then syncs inside the lock as before.
        description.Flags = 0;
        swapChainFlags_ = 0;
        hr = factory->CreateSwapChainForHwnd(
            device_.Get(), window, &description, nullptr, nullptr, &swapChain_);
    }
    if (FAILED(hr)) {
        setError("CreateSwapChainForHwnd", hr);
        return false;
    }
    // The app translates Alt+Enter itself; DXGI's own handling would fight it.
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (swapChainFlags_ & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        Microsoft::WRL::ComPtr<IDXGISwapChain2> waitableSwapChain;
        if (SUCCEEDED(swapChain_.As(&waitableSwapChain))) {
            waitableSwapChain->SetMaximumFrameLatency(1);
            frameLatencyWaitable_ = waitableSwapChain->GetFrameLatencyWaitableObject();
        }
    }

    hr = device_.As(&videoDevice_);
    if (FAILED(hr)) {
        setError("Query ID3D11VideoDevice", hr);
        return false;
    }
    hr = context_.As(&videoContext_);
    if (FAILED(hr)) {
        setError("Query ID3D11VideoContext", hr);
        return false;
    }
    if (FAILED(context_.As(&videoContext1_))) videoContext1_.Reset();
    probeVideoEnhancements();
    refreshDisplayStatus();
    return true;
}

void D3DRenderer::probeVideoEnhancements() {
    enhancementStatus_.superResolutionSupported = false;
    enhancementStatus_.rtxHdrSupported = false;
    if (!enhancementStatus_.nvidiaAdapter || !videoDevice_ || !videoContext_) return;
    // A throwaway 1080p processor is enough to ask the driver; the answer is
    // per device, not per size.
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = 1920;
    content.InputHeight = 1080;
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = 1920;
    content.OutputHeight = 1080;
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&content, &enumerator)) ||
        FAILED(videoDevice_->CreateVideoProcessor(enumerator.Get(), 0, &processor))) {
        appendDiagnostic("RTX Video probe: no video processor could be created");
        return;
    }
    NvidiaSuperResolutionExtension superResolution{};
    superResolution.enable = 1;
    HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
        processor.Get(), 0, &kNvidiaSuperResolutionInterface,
        sizeof(superResolution), &superResolution);
    enhancementStatus_.superResolutionSupported = SUCCEEDED(hr);
    // The RTX Video HDR interface answers a capability query as well as
    // taking the enable; mpv asks first and so do we.
    UINT trueHdrSupported = 0;
    const HRESULT queried = videoContext_->VideoProcessorGetStreamExtension(
        processor.Get(), 0, &kNvidiaTrueHdrInterface, sizeof(trueHdrSupported), &trueHdrSupported);
    enhancementStatus_.rtxHdrSupported = SUCCEEDED(queried) && trueHdrSupported != 0;
    std::ostringstream out;
    out << "RTX Video probe: Super Resolution "
        << (enhancementStatus_.superResolutionSupported ? "accepted" : "declined") << " (0x"
        << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(hr)
        << "), RTX Video HDR " << (enhancementStatus_.rtxHdrSupported ? "supported" : "unsupported")
        << " (0x" << std::setw(8) << static_cast<unsigned long>(queried) << ", "
        << std::dec << trueHdrSupported << ")";
    appendDiagnostic(out.str());
}

void D3DRenderer::refreshDisplayStatus() {
    framesSinceDisplayStatus_ = 0;
    bool displayHdr = false;
    float sdrWhiteNits = 200.0F;
    const HMONITOR monitor = window_ ? MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST) : nullptr;
    std::wstring outputName;
    if (adapter_ && monitor) {
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter_->EnumOutputs(index, &output) != S_OK) break;
            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 description{};
            if (FAILED(output.As(&output6)) || FAILED(output6->GetDesc1(&description)) ||
                description.Monitor != monitor) continue;
            displayHdr = description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            outputName = description.DeviceName;
            break;
        }
    }
    // Windows' SDR white level for that output, the brightness it gives SDR
    // windows on an HDR display; the interface layer is drawn to match it.
    if (!outputName.empty()) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) == ERROR_SUCCESS) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                   &modeCount, modes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT32 index = 0; index < pathCount; ++index) {
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
                    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source.header.size = sizeof(source);
                    source.header.adapterId = paths[index].sourceInfo.adapterId;
                    source.header.id = paths[index].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                        outputName != source.viewGdiDeviceName) continue;
                    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
                    white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                    white.header.size = sizeof(white);
                    white.header.adapterId = paths[index].targetInfo.adapterId;
                    white.header.id = paths[index].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0) {
                        // Reported in thousandths of the 80-nit reference.
                        sdrWhiteNits = std::clamp(white.SDRWhiteLevel / 1000.0F * 80.0F, 80.0F, 1000.0F);
                    }
                    break;
                }
            }
        }
    }
    if (displayHdr != enhancementStatus_.displayHdr ||
        std::abs(sdrWhiteNits - enhancementStatus_.sdrWhiteNits) > 0.5F) {
        std::ostringstream out;
        out << "Display: " << narrow(outputName) << (displayHdr ? " HDR on" : " HDR off")
            << ", SDR white " << sdrWhiteNits << " nits";
        appendDiagnostic(out.str());
    }
    enhancementStatus_.displayHdr = displayHdr;
    enhancementStatus_.sdrWhiteNits = sdrWhiteNits;
}

bool D3DRenderer::wantHdrOutput() const {
    return enhancements_.rtxHdr && enhancementStatus_.nvidiaAdapter &&
           enhancementStatus_.rtxHdrSupported && enhancementStatus_.displayHdr &&
           videoContext1_ != nullptr;
}

bool D3DRenderer::applyOutputMode() {
    const bool wanted = wantHdrOutput();
    if (wanted == hdrOutput_ || !swapChain_ || deviceLost_) return wanted == hdrOutput_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    if (FAILED(swapChain_.As(&swapChain3))) {
        appendDiagnostic("RTX Video HDR: swap chain has no colour-space control; staying 8-bit");
        return false;
    }
    const DXGI_FORMAT format = wanted ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    const DXGI_COLOR_SPACE_TYPE colorSpace = wanted
        ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support = 0;
    if (wanted && (FAILED(swapChain3->CheckColorSpaceSupport(colorSpace, &support)) ||
                   !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))) {
        appendDiagnostic("RTX Video HDR: the swap chain cannot present PQ here; staying 8-bit");
        return false;
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    HRESULT hr = swapChain_->ResizeBuffers(0, width_, height_, format, swapChainFlags_);
    if (FAILED(hr)) {
        noteFailure("ResizeBuffers(format)", hr);
        // Try to get the previous chain back rather than leave no back buffer.
        swapChain_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
        createBackBuffer();
        return false;
    }
    hr = swapChain3->SetColorSpace1(colorSpace);
    if (FAILED(hr)) setError("SetColorSpace1", hr);
    hdrOutput_ = wanted;
    enhancementStatus_.hdrOutput = wanted;
    // Every slot's output texture and processor were built for the old format.
    slots_ = {};
    softwareUploads_ = {};
    softwarePq_ = {};
    loggedSoftwareRoute_.fill(-1);
    createBackBuffer();
    appendDiagnostic(wanted ? "Output: 10-bit PQ (RTX Video HDR)" : "Output: 8-bit sRGB");
    return true;
}

void D3DRenderer::setVideoEnhancements(const VideoEnhancementSettings& settings) {
    std::scoped_lock lock(deviceMutex_);
    if (settings == enhancements_) return;
    enhancements_ = settings;
    // Rebuild every processor with the new extension state on the next frame.
    slots_ = {};
    softwareUploads_ = {};
    softwarePq_ = {};
    loggedSoftwareRoute_.fill(-1);
    if (!deviceLost_ && swapChain_) {
        refreshDisplayStatus();
        applyOutputMode();
    }
}

VideoEnhancementStatus D3DRenderer::enhancementStatus() const {
    std::scoped_lock lock(deviceMutex_);
    return enhancementStatus_;
}

void D3DRenderer::applyStreamExtensions(std::size_t slot, bool hdrSource) {
    auto& target = slots_[slot];
    target.rtxHdrApplied = false;
    if (!enhancementStatus_.nvidiaAdapter) return;
    // Super Resolution upscales; asked for on a stream shown at or below
    // its own size the driver still runs its network and, at 4K60, that
    // alone can take most of a frame. So it is set only when the processor
    // magnifies, and a source shown at its own size is left alone.
    const bool magnifies = target.outputWidth > target.inputWidth || target.outputHeight > target.inputHeight;
    if (enhancements_.superResolution && enhancementStatus_.superResolutionSupported && magnifies) {
        NvidiaSuperResolutionExtension extension{};
        extension.enable = 1;
        const HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
            target.processor.Get(), 0, &kNvidiaSuperResolutionInterface, sizeof(extension), &extension);
        enhancementStatus_.superResolutionApplied = SUCCEEDED(hr);
        const int outcome = SUCCEEDED(hr) ? 1 : 0;
        if (outcome != loggedSuperResolution_) {
            loggedSuperResolution_ = outcome;
            std::ostringstream out;
            out << "RTX Super Resolution " << (outcome ? "enabled" : "refused") << " on "
                << target.inputWidth << "x" << target.inputHeight << " -> "
                << target.outputWidth << "x" << target.outputHeight;
            if (!outcome) out << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            appendDiagnostic(out.str());
        }
    } else if (enhancements_.superResolution && enhancementStatus_.superResolutionSupported) {
        enhancementStatus_.superResolutionApplied = false;
        if (loggedSuperResolution_ != 2) {
            loggedSuperResolution_ = 2;
            std::ostringstream out;
            out << "RTX Super Resolution idle: " << target.inputWidth << "x" << target.inputHeight
                << " shown at " << target.outputWidth << "x" << target.outputHeight << ", no upscaling";
            appendDiagnostic(out.str());
        }
    } else {
        enhancementStatus_.superResolutionApplied = false;
        loggedSuperResolution_ = -1;
    }
    // RTX Video HDR only makes sense with a PQ output to write into, and
    // only for an SDR stream; an HDR10 stream is passed through as it is.
    if (hdrOutput_ && enhancements_.rtxHdr && !hdrSource) {
        NvidiaTrueHdrExtension extension{};
        extension.enable = 1;
        extension.reserved = 0;
        const HRESULT hr = videoContext_->VideoProcessorSetStreamExtension(
            target.processor.Get(), 0, &kNvidiaTrueHdrInterface, sizeof(extension), &extension);
        enhancementStatus_.rtxHdrApplied = SUCCEEDED(hr);
        target.rtxHdrApplied = SUCCEEDED(hr);
        const int outcome = SUCCEEDED(hr) ? 1 : 0;
        if (outcome != loggedRtxHdr_) {
            loggedRtxHdr_ = outcome;
            std::ostringstream out;
            out << "RTX Video HDR " << (outcome ? "enabled" : "refused");
            if (!outcome) out << " (0x" << std::hex << static_cast<unsigned long>(hr) << ")";
            appendDiagnostic(out.str());
        }
    } else if (hdrOutput_) {
        enhancementStatus_.rtxHdrApplied = false;
        if (loggedRtxHdr_ != 2) {
            loggedRtxHdr_ = 2;
            appendDiagnostic("HDR10 source passed through as PQ; RTX Video HDR not applied to it");
        }
    } else {
        enhancementStatus_.rtxHdrApplied = false;
        loggedRtxHdr_ = -1;
    }
}

bool D3DRenderer::createPipeline() {
    Microsoft::WRL::ComPtr<ID3DBlob> vertexBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(
        kVertexShader, sizeof(kVertexShader) - 1, "QuadDeckVS", nullptr, nullptr,
        "main", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBlob, &errors);
    if (FAILED(hr)) {
        setError("D3DCompile(vertex)", hr);
        return false;
    }
    hr = device_->CreateVertexShader(
        vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, &vertexShader_);
    if (FAILED(hr)) {
        setError("CreateVertexShader", hr);
        return false;
    }
    D3D11_BUFFER_DESC transformDescription{};
    transformDescription.ByteWidth = sizeof(float) * 4;
    transformDescription.Usage = D3D11_USAGE_DEFAULT;
    transformDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&transformDescription, nullptr, &uvTransformBuffer_);
    if (FAILED(hr)) {
        setError("Create UV transform buffer", hr);
        return false;
    }
    D3D11_BUFFER_DESC pixelParamsDescription{};
    pixelParamsDescription.ByteWidth = sizeof(float) * 12;
    pixelParamsDescription.Usage = D3D11_USAGE_DEFAULT;
    pixelParamsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&pixelParamsDescription, nullptr, &pixelParamsBuffer_);
    if (FAILED(hr)) {
        setError("Create PotPlayer shader parameter buffer", hr);
        return false;
    }
    D3D11_BUFFER_DESC vibranceDescription{};
    vibranceDescription.ByteWidth = sizeof(float) * 4;
    vibranceDescription.Usage = D3D11_USAGE_DEFAULT;
    vibranceDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    const auto vibrance = clampSmartVibranceSettings(smartVibranceSettings_);
    const float vibranceValues[] = {
        vibrance.intensity, vibrance.saturationPivot,
        vibrance.grayPivot, vibrance.graySharpness};
    D3D11_SUBRESOURCE_DATA vibranceData{};
    vibranceData.pSysMem = vibranceValues;
    hr = device_->CreateBuffer(
        &vibranceDescription, &vibranceData, &smartVibranceBuffer_);
    if (FAILED(hr)) {
        setError("Create Smart Vibrance parameter buffer", hr);
        return false;
    }
    if (!compilePixelShader(kPixelShader, sizeof(kPixelShader) - 1, "QuadDeckPS")) return false;
    copyShader_ = pixelShader_;
    if (!compilePixelShader(kSdrToPqShader, sizeof(kSdrToPqShader) - 1, "SdrToPq")) return false;
    sdrToPqShader_ = pixelShader_;
    pixelShader_ = copyShader_;
    hr = device_->CreateBuffer(&vibranceDescription, nullptr, &sdrToPqParams_);
    if (FAILED(hr)) { setError("Create SDR presentation parameter buffer", hr); return false; }
    hr = device_->CreateBuffer(&vibranceDescription, nullptr, &vibranceColorBuffer_);
    if (FAILED(hr)) { setError("Create Vibrance colour-space buffer", hr); return false; }
    {
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        Microsoft::WRL::ComPtr<ID3DBlob> compositeErrors;
        hr = D3DCompile(kOverlayCompositeShader, sizeof(kOverlayCompositeShader) - 1,
                        "OverlayComposite", nullptr, nullptr, "main", "ps_4_0",
                        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &compositeErrors);
        if (FAILED(hr)) {
            if (compositeErrors) {
                error_.assign(static_cast<const char*>(compositeErrors->GetBufferPointer()),
                              compositeErrors->GetBufferSize());
            } else {
                setError("D3DCompile(overlay composite)", hr);
            }
            return false;
        }
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                        nullptr, &overlayCompositeShader_);
        if (FAILED(hr)) { setError("Create overlay composite shader", hr); return false; }
        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device_->CreateBlendState(&blend, &overlayBlend_);
        if (FAILED(hr)) { setError("Create overlay blend state", hr); return false; }
        D3D11_BUFFER_DESC paramsDescription{};
        paramsDescription.ByteWidth = sizeof(float) * 4;
        paramsDescription.Usage = D3D11_USAGE_DEFAULT;
        paramsDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&paramsDescription, nullptr, &overlayParamsBuffer_);
        if (FAILED(hr)) { setError("Create overlay parameter buffer", hr); return false; }
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sampler, &sampler_);
    if (FAILED(hr)) {
        setError("CreateSamplerState", hr);
        return false;
    }
    return true;
}

bool D3DRenderer::compilePixelShader(const char* source, std::size_t length, const char* name) {
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr, "main", "ps_4_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) error_.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        else setError("D3DCompile(pixel)", hr);
        return false;
    }
    std::scoped_lock lock(deviceMutex_);
    if (!device_ || (deviceLost_ && !deviceRebuildInProgress_)) {
        error_ = "Cannot create pixel shader while graphics device is unavailable";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    const HRESULT createResult = device_->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
    if (FAILED(createResult)) { setError("CreatePixelShader", createResult); return false; }
    pixelShader_ = std::move(shader);
    smartVibranceSelected_ = false;
    error_.clear();
    return true;
}

bool D3DRenderer::setShaderPreset(ShaderPreset preset) {
    std::scoped_lock lock(deviceMutex_);
    switch (preset) {
    case ShaderPreset::Sharpen: return compilePixelShader(kSharpenShader, sizeof(kSharpenShader) - 1, "Sharpen");
    case ShaderPreset::Grayscale: return compilePixelShader(kGrayscaleShader, sizeof(kGrayscaleShader) - 1, "Grayscale");
    case ShaderPreset::Invert: return compilePixelShader(kInvertShader, sizeof(kInvertShader) - 1, "Invert");
    case ShaderPreset::SmartVibrancePlus:
        if (!compilePixelShader(kSmartVibrancePlusShader,
                                sizeof(kSmartVibrancePlusShader) - 1,
                                "SmartVibrancePlus")) return false;
        smartVibranceSelected_ = true;
        return true;
    default: return compilePixelShader(kPixelShader, sizeof(kPixelShader) - 1, "Normal");
    }
}

void D3DRenderer::setSmartVibranceSettings(const SmartVibranceSettings& settings) {
    std::scoped_lock lock(deviceMutex_);
    smartVibranceSettings_ = clampSmartVibranceSettings(settings);
    if (!context_ || !smartVibranceBuffer_ || deviceLost_) return;
    const float values[] = {
        smartVibranceSettings_.intensity,
        smartVibranceSettings_.saturationPivot,
        smartVibranceSettings_.grayPivot,
        smartVibranceSettings_.graySharpness};
    context_->UpdateSubresource(smartVibranceBuffer_.Get(), 0, nullptr, values, 0, 0);
}

bool D3DRenderer::loadPixelShader(const std::wstring& path) {
    return compilePixelShaderFile(path);
}

bool D3DRenderer::compilePixelShaderFile(const std::wstring& path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) {
        error_ = "Could not open pixel shader file";
        return false;
    }
    std::ostringstream bytes;
    bytes << input.rdbuf();
    const std::string source = adaptPotPlayerShader(bytes.str());
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "PotPlayer-compatible pixel shader",
        nullptr, nullptr, "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) error_.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        else setError("Compile pixel shader file", hr);
        return false;
    }
    std::scoped_lock lock(deviceMutex_);
    if (!device_ || (deviceLost_ && !deviceRebuildInProgress_)) {
        error_ = "Cannot create pixel shader while graphics device is unavailable";
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11PixelShader> shader;
    const HRESULT createResult = device_->CreatePixelShader(
        blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &shader);
    if (FAILED(createResult)) { setError("Create external pixel shader", createResult); return false; }
    pixelShader_ = std::move(shader);
    smartVibranceSelected_ = false;
    error_.clear();
    return true;
}

void D3DRenderer::bindVibranceColorSpace(std::size_t slot) {
    ID3D11Buffer* buffer = nullptr;
    if (smartVibranceSelected_) {
        const auto& target = slots_[slot];
        const bool pq = hdrOutput_ && (target.hdrSource ||
            (target.hardware && target.outputFormat == DXGI_FORMAT_R10G10B10A2_UNORM));
        const float mode = !pq ? 0.0F :
            (target.rtxHdrApplied && !target.hdrSource ? 1.0F : 2.0F);
        const float values[] = {mode, 0.0F, 0.0F, 0.0F};
        context_->UpdateSubresource(vibranceColorBuffer_.Get(), 0, nullptr, values, 0, 0);
        buffer = vibranceColorBuffer_.Get();
    }
    // b0/b1 retain their existing contracts; custom shaders get no new b2.
    context_->PSSetConstantBuffers(2, 1, &buffer);
}

bool D3DRenderer::createBackBuffer() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) {
        noteFailure("GetBuffer", hr);
        return false;
    }
    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget_);
    if (FAILED(hr)) {
        setError("CreateRenderTargetView", hr);
        return false;
    }
    // The interface layer follows the back buffer's size; it is rebuilt on
    // demand by the first PQ frame that needs it.
    overlayLayerView_.Reset();
    overlayLayer_.Reset();
    return true;
}

bool D3DRenderer::compositeOverlayLayer() {
    if (!overlayLayer_) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = width_;
        description.Height = height_;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        HRESULT hr = device_->CreateTexture2D(&description, nullptr, &overlayLayer_);
        if (FAILED(hr)) { setError("Create interface layer", hr); return false; }
        hr = device_->CreateShaderResourceView(overlayLayer_.Get(), nullptr, &overlayLayerView_);
        if (FAILED(hr)) { setError("Create interface layer view", hr); overlayLayer_.Reset(); return false; }
    }
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(overlayLayer_.As(&surface))) return false;
    overlay_(surface.Get(), true);
    // Direct2D leaves its own state behind; bind everything this pass needs.
    context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width_);
    viewport.Height = static_cast<float>(height_);
    viewport.MaxDepth = 1.0F;
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    const float identity[] = {1.0F, 1.0F, 0.0F, 0.0F};
    context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, identity, 0, 0);
    const float params[] = {enhancementStatus_.sdrWhiteNits, 0.0F, 0.0F, 0.0F};
    context_->UpdateSubresource(overlayParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
    context_->PSSetShader(overlayCompositeShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, overlayParamsBuffer_.GetAddressOf());
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    ID3D11ShaderResourceView* view = overlayLayerView_.Get();
    context_->PSSetShaderResources(0, 1, &view);
    context_->OMSetBlendState(overlayBlend_.Get(), nullptr, 0xFFFFFFFF);
    context_->Draw(4, 0);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    return true;
}

void D3DRenderer::resize(unsigned width, unsigned height) {
    if (!swapChain_ || width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }
    std::scoped_lock deviceLock(deviceMutex_);
    width_ = width;
    height_ = height;
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    renderTarget_.Reset();
    const HRESULT hr = swapChain_->ResizeBuffers(
        0, width_, height_, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) {
        noteFailure("ResizeBuffers", hr);
        return;
    }
    createBackBuffer();
    // A resize often means the window moved; the output it sits on may differ.
    refreshDisplayStatus();
    applyOutputMode();
}

bool D3DRenderer::createSlotResources(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    auto& target = slots_[slot];
    D3D11_TEXTURE2D_DESC inputDescription{};
    frame.texture->GetDesc(&inputDescription);
    const int outputWidth = requested.width;
    const int outputHeight = requested.height;
    const auto color = videoColorDescription(frame.ownerFrame, frame.width, frame.height);
    const bool hdrSource = hdrOutput_ && color.pq;
    if (target.texture && target.inputWidth == frame.width && target.inputHeight == frame.height &&
        target.inputFormat == inputDescription.Format && target.outputWidth == outputWidth &&
        target.outputHeight == outputHeight && target.hdrOutputMode == hdrOutput_ &&
        target.hdrSource == hdrSource) {
        return true;
    }

    target = SlotTexture{};
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
    content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    content.InputFrameRate = {60, 1};
    content.InputWidth = static_cast<UINT>(frame.width);
    content.InputHeight = static_cast<UINT>(frame.height);
    content.OutputFrameRate = {60, 1};
    content.OutputWidth = static_cast<UINT>(outputWidth);
    content.OutputHeight = static_cast<UINT>(outputHeight);
    content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    HRESULT hr = videoDevice_->CreateVideoProcessorEnumerator(&content, &target.enumerator);
    if (FAILED(hr)) { setError("CreateVideoProcessorEnumerator", hr); return false; }
    UINT inputSupport{};
    hr = target.enumerator->CheckVideoProcessorFormat(inputDescription.Format, &inputSupport);
    if (FAILED(hr) || !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
        setError("D3D11 Video Processor input format unsupported", FAILED(hr) ? hr : E_FAIL);
        return false;
    }
    hr = videoDevice_->CreateVideoProcessor(target.enumerator.Get(), 0, &target.processor);
    if (FAILED(hr)) { setError("CreateVideoProcessor", hr); return false; }
    target.inputWidth = frame.width;
    target.inputHeight = frame.height;
    target.outputWidth = outputWidth;
    target.outputHeight = outputHeight;
    applyStreamExtensions(slot, hdrSource);
    // Some drivers accept the SDR/PQ colour-space pair but merely copy SDR
    // codes when TrueHDR is disabled/refused. Use a known SDR processor output
    // plus our explicit presentation transform in that case, for both decode
    // paths. Native PQ is still passed through without another SDR transform.
    const DXGI_FORMAT outputFormat = hdrOutput_ && (hdrSource || target.rtxHdrApplied)
        ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT outputSupport{};
    hr = target.enumerator->CheckVideoProcessorFormat(outputFormat, &outputSupport);
    if (FAILED(hr) || !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        setError("D3D11 Video Processor output format unsupported", FAILED(hr) ? hr : E_FAIL);
        return false;
    }

    D3D11_TEXTURE2D_DESC outputDescription{};
    outputDescription.Width = static_cast<UINT>(outputWidth);
    outputDescription.Height = static_cast<UINT>(outputHeight);
    outputDescription.MipLevels = 1;
    outputDescription.ArraySize = 1;
    outputDescription.Format = outputFormat;
    outputDescription.SampleDesc.Count = 1;
    outputDescription.Usage = D3D11_USAGE_DEFAULT;
    outputDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    hr = device_->CreateTexture2D(&outputDescription, nullptr, &target.texture);
    if (FAILED(hr)) { setError("Create zero-copy output texture", hr); return false; }
    hr = device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.view);
    if (FAILED(hr)) { setError("Create output shader view", hr); return false; }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDescription{};
    outputViewDescription.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    outputViewDescription.Texture2D.MipSlice = 0;
    hr = videoDevice_->CreateVideoProcessorOutputView(
        target.texture.Get(), target.enumerator.Get(), &outputViewDescription, &target.outputView);
    if (FAILED(hr)) { setError("CreateVideoProcessorOutputView", hr); return false; }
    target.inputFormat = inputDescription.Format;
    target.outputFormat = outputFormat;
    target.hdrSource = hdrSource;
    target.hdrOutputMode = hdrOutput_;
    return true;
}

bool D3DRenderer::convertSurface(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    auto& target = slots_[slot];
    if (!createSlotResources(slot, frame, requested)) return false;
    // Metadata belongs to each decoded frame, not the cached texture geometry.
    const auto color = videoColorDescription(frame.ownerFrame, frame.width, frame.height);
    if (target.outputFormat == DXGI_FORMAT_R10G10B10A2_UNORM) {
        // The PQ output mode names the spaces by DXGI type: an HDR10 stream is
        // handed over as PQ BT.2020 and passed through, anything else as the
        // SDR space it is and left to RTX Video HDR to expand. A BT.2020 SDR
        // stream is the one case the 8-bit path approximates that this one
        // converts properly.
        DXGI_COLOR_SPACE_TYPE inputSpace;
        if (color.pq) {
            inputSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        } else if (color.bt2020) {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        } else if (color.matrix == VideoColorMatrix::Bt709) {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
        } else {
            inputSpace = color.fullRange ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601
                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
        }
        videoContext1_->VideoProcessorSetStreamColorSpace1(target.processor.Get(), 0, inputSpace);
        videoContext1_->VideoProcessorSetOutputColorSpace1(
            target.processor.Get(), DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor{};
        inputColor.YCbCr_Matrix = color.matrix == VideoColorMatrix::Bt709 ? 1 : 0;
        inputColor.RGB_Range = color.fullRange ? 0 : 1;
        inputColor.Nominal_Range = color.fullRange
            ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
            : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor{};
        outputColor.RGB_Range = 0;
        outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        videoContext_->VideoProcessorSetStreamColorSpace(target.processor.Get(), 0, &inputColor);
        videoContext_->VideoProcessorSetOutputColorSpace(target.processor.Get(), &outputColor);
    }
    // Keep driver automatic enhancements independent of the selected shader
    // from silently changing the matrix/range conversion's output pixels.
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(target.processor.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDescription{};
    inputDescription.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    inputDescription.Texture2D.MipSlice = 0;
    inputDescription.Texture2D.ArraySlice = frame.arraySlice;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    HRESULT hr = videoDevice_->CreateVideoProcessorInputView(
        frame.texture, target.enumerator.Get(), &inputDescription, &inputView);
    if (FAILED(hr)) { setError("CreateVideoProcessorInputView", hr); return false; }
    RECT source{0, 0, frame.width, frame.height};
    RECT destination{0, 0, target.outputWidth, target.outputHeight};
    videoContext_->VideoProcessorSetStreamFrameFormat(
        target.processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(target.processor.Get(), 0, TRUE, &source);
    videoContext_->VideoProcessorSetStreamDestRect(target.processor.Get(), 0, TRUE, &destination);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();
    hr = videoContext_->VideoProcessorBlt(
        target.processor.Get(), target.outputView.Get(), 0, 1, &stream);
    if (FAILED(hr)) { noteFailure("VideoProcessorBlt", hr); return false; }
    target.serial = frame.serial;
    target.hardware = true;
    return true;
}

bool D3DRenderer::convertSoftwareSurface(
    std::size_t slot, const HardwareVideoFrame& frame, const SurfaceSize& requested) {
    if (!videoDevice_ || !videoContext_ || !frame.ownerFrame ||
        !softwareNv12Eligible(*frame.ownerFrame)) return false;
    auto& upload = softwareUploads_[slot];
    const auto& source = *frame.ownerFrame;
    if (upload.width != source.width || upload.height != source.height) {
        upload = {};
        upload.width = source.width;
        upload.height = source.height;
    }
    // Retry after a size/output/settings/device change, not on every video
    // frame when a driver rejects a particular processing configuration.
    if (upload.failed && upload.failedOutputWidth == requested.width &&
        upload.failedOutputHeight == requested.height) return false;
    upload.failed = false;
    const auto fail = [&] {
        upload.failed = true;
        upload.failedOutputWidth = requested.width;
        upload.failedOutputHeight = requested.height;
        appendDiagnostic("Software NV12 video processor fallback: pane=" +
            std::to_string(slot + 1) + " input=" + std::to_string(source.width) + "x" +
            std::to_string(source.height) + " output=" + std::to_string(requested.width) + "x" +
            std::to_string(requested.height) + " reason=" + error_);
        return false;
    };
    if (!upload.texture) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(source.width);
        description.Height = static_cast<UINT>(source.height);
        description.MipLevels = description.ArraySize = description.SampleDesc.Count = 1;
        description.Format = DXGI_FORMAT_NV12;
        description.Usage = D3D11_USAGE_DEFAULT;
        // This is a processor input, independent of the codec's decoder caps.
        description.BindFlags = D3D11_BIND_RENDER_TARGET;
        const HRESULT hr = device_->CreateTexture2D(&description, nullptr, &upload.texture);
        if (FAILED(hr)) { setError("Create software NV12 input", hr); return fail(); }
    }
    if (upload.serial != frame.serial) {
        int stride{};
        if (!packSoftwareNv12(source, upload.bytes, stride)) return false;
        context_->UpdateSubresource(upload.texture.Get(), 0, nullptr,
                                    upload.bytes.data(), static_cast<UINT>(stride), 0);
        ++stats_.softwareNv12Uploads;
        upload.serial = frame.serial;
    }
    HardwareVideoFrame uploaded;
    uploaded.ownerFrame = av_frame_clone(frame.ownerFrame);
    if (!uploaded.ownerFrame) { error_ = "Cannot retain software colour metadata"; return fail(); }
    uploaded.texture = upload.texture.Get();
    uploaded.width = source.width;
    uploaded.height = source.height;
    uploaded.serial = frame.serial;
    if (!convertSurface(slot, uploaded, requested)) return fail();
    // Slot.hardware means a video-processor output; the public frame still
    // accurately says software decode. Metadata, range and colour stay intact.
    return true;
}

bool D3DRenderer::uploadSoftwareFrame(std::size_t slot, const HardwareVideoFrame& frame) {
    auto& target = slots_[slot];
    if (target.texture && !target.hardware && target.serial == frame.serial &&
        target.outputWidth == frame.width && target.outputHeight == frame.height) return true;
    if (!target.texture || target.hardware || target.outputWidth != frame.width ||
        target.outputHeight != frame.height) {
        target = SlotTexture{};
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(frame.width);
        description.Height = static_cast<UINT>(frame.height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&description, nullptr, &target.texture);
        if (FAILED(hr)) { setError("Create software frame texture", hr); return false; }
        hr = device_->CreateShaderResourceView(target.texture.Get(), nullptr, &target.view);
        if (FAILED(hr)) { setError("Create software frame view", hr); return false; }
        target.outputWidth = frame.width;
        target.outputHeight = frame.height;
        target.outputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        target.hardware = false;
    }
    context_->UpdateSubresource(target.texture.Get(), 0, nullptr, frame.bgra.data(), frame.stride, 0);
    ++stats_.softwareFrameUploads;
    // swscale only changes matrix/range; it does not turn PQ/HLG into SDR.
    // Keep those pre-existing limited software paths out of the SDR transform
    // and SDR Vibrance. Correct native HDR software tone mapping is separate.
    target.hdrSource = frame.ownerFrame &&
        (frame.ownerFrame->color_trc == AVCOL_TRC_SMPTE2084 ||
         frame.ownerFrame->color_trc == AVCOL_TRC_ARIB_STD_B67);
    target.serial = frame.serial;
    return true;
}

bool D3DRenderer::prepareSoftwarePqTexture(std::size_t slot) {
    const auto& source = slots_[slot];
    auto& target = softwarePq_[slot];
    if (!source.view || source.outputFormat != DXGI_FORMAT_B8G8R8A8_UNORM ||
        source.hdrSource || !hdrOutput_) return false;
    if (target.width != source.outputWidth || target.height != source.outputHeight) target = {};
    if (!target.pq) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(source.outputWidth);
        desc.Height = static_cast<UINT>(source.outputHeight);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const auto create = [&](auto& texture, auto& view, auto& rtv) {
            HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture);
            if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texture.Get(), nullptr, &view);
            if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(texture.Get(), nullptr, &rtv);
            if (FAILED(hr)) setError("Create software PQ presentation texture", hr);
            return SUCCEEDED(hr);
        };
        if (!create(target.effects, target.effectsView, target.effectsTarget) ||
            !create(target.pq, target.pqView, target.pqTarget)) { target = {}; return false; }
        target.width = source.outputWidth;
        target.height = source.outputHeight;
    }
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    const float identity[] = {1, 1, 0, 0};
    context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, identity, 0, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(target.width);
    viewport.Height = static_cast<float>(target.height);
    viewport.MaxDepth = 1;
    context_->RSSetViewports(1, &viewport);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    // Run every selected effect, including user shaders, in SDR before the
    // output transform. Per-frame execution preserves temporal shader inputs.
    const float clock = static_cast<float>(std::fmod(std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
    const float params[] = {viewport.Width, viewport.Height,
        static_cast<float>(source.serial), clock, 1 / viewport.Width, 1 / viewport.Height,
        0, 0, 0, 0, 0, 0};
    context_->UpdateSubresource(pixelParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
    context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());
    bindVibranceColorSpace(slot);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->OMSetRenderTargets(1, target.effectsTarget.GetAddressOf(), nullptr);
    context_->PSSetShaderResources(0, 1, source.view.GetAddressOf());
    context_->Draw(4, 0);
    ID3D11ShaderResourceView* empty = nullptr;
    context_->PSSetShaderResources(0, 1, &empty);
    context_->OMSetRenderTargets(1, target.pqTarget.GetAddressOf(), nullptr);
    context_->PSSetShaderResources(0, 1, target.effectsView.GetAddressOf());
    const float white[] = {std::clamp(enhancementStatus_.sdrWhiteNits, 80.0F, 1000.0F), 0, 0, 0};
    context_->UpdateSubresource(sdrToPqParams_.Get(), 0, nullptr, white, 0, 0);
    context_->PSSetConstantBuffers(0, 1, sdrToPqParams_.GetAddressOf());
    context_->PSSetShader(sdrToPqShader_.Get(), nullptr, 0);
    context_->Draw(4, 0);
    context_->PSSetShaderResources(0, 1, &empty);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

void D3DRenderer::render(
    const PaneArray<std::shared_ptr<const HardwareVideoFrame>>& frames,
    const PaneArray<PaneView>& views,
    const PaneArray<bool>& active, LayoutMode layout,
    int expandedPane, int soloPane, unsigned contentHeight,
    const PaneArray<float>& aspectRatios,
    AutoLayoutFocus autoFocus, int autoFocusPane) {
    if (!renderTarget_ || width_ == 0 || height_ == 0 || deviceLost_) {
        return;
    }
    using PerfClock = std::chrono::steady_clock;
    const auto lockRequested = PerfClock::now();
    std::unique_lock deviceLock(deviceMutex_);
    const auto lockAcquired = PerfClock::now();
    if (!frameQuery_ && device_) {
        D3D11_QUERY_DESC query{};
        query.Query = D3D11_QUERY_EVENT;
        if (FAILED(device_->CreateQuery(&query, &frameQuery_))) frameQuery_.Reset();
    }
    double convertMs = 0.0;
    // HDR can be switched in Windows, or the window carried to another
    // display, while playing; look every couple of seconds.
    if (++framesSinceDisplayStatus_ >= 120) {
        refreshDisplayStatus();
        applyOutputMode();
        if (!renderTarget_) return;
    }
    // Only the hardware path goes through the video processor, so only it
    // can hand Super Resolution the upscale.
    const bool magnifyForSuperResolution = enhancements_.superResolution &&
                                           enhancementStatus_.superResolutionSupported;
    constexpr float background[] = {0.012F, 0.014F, 0.020F, 1.0F};
    context_->ClearRenderTargetView(renderTarget_.Get(), background);
    context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, uvTransformBuffer_.GetAddressOf());
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetSamplers(0, 1, sampler_.GetAddressOf());
    context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
    context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());

    const float videoHeight = static_cast<float>(std::min(height_, std::max(1U, contentHeight)));
    const auto cells = activeLayoutCells(
        static_cast<float>(width_), videoHeight, active, layout, expandedPane, soloPane,
        aspectRatios, autoFocus, autoFocusPane);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const auto& frame = frames[index];
        if (!frame || cells[index].width <= 0 || cells[index].height <= 0) {
            continue;
        }
        const float paneZoom = std::clamp(views[index].zoom, 1.0F, 4.0F);
        const bool softwareVideoProcessor = !frame->hardware && frame->ownerFrame &&
            (hdrOutput_ || enhancements_.superResolution) &&
            softwareNv12Eligible(*frame->ownerFrame);
        const int sourceWidth = softwareVideoProcessor ? frame->ownerFrame->width : frame->width;
        const int sourceHeight = softwareVideoProcessor ? frame->ownerFrame->height : frame->height;
        const auto requested = presentationSize(
            sourceWidth, sourceHeight,
            cells[index].width * paneZoom, cells[index].height * paneZoom,
            3840, 8, magnifyForSuperResolution && (frame->hardware || softwareVideoProcessor));
        if (slots_[index].serial != frame->serial ||
            ((frame->hardware || softwareVideoProcessor) && (slots_[index].outputWidth != requested.width ||
                                 slots_[index].outputHeight != requested.height))) {
            const auto convertStarted = PerfClock::now();
            const auto previousError = error_;
            bool converted = frame->hardware ? convertSurface(index, *frame, requested)
                : softwareVideoProcessor && convertSoftwareSurface(index, *frame, requested);
            if (!frame->hardware && !converted && !deviceLost_) {
                converted = uploadSoftwareFrame(index, *frame);
                // The rejected processor route has been logged. A successful
                // fallback is a usable frame, not a persistent renderer error.
                if (converted) error_ = previousError;
            }
            convertMs += std::chrono::duration<double, std::milli>(
                PerfClock::now() - convertStarted).count();
            if (!converted) continue;
            if (!frame->hardware) {
                const int route = slots_[index].hardware ? (slots_[index].rtxHdrApplied ? 2 : 1)
                    : (hdrOutput_ ? (slots_[index].hdrSource ? 4 : 3) : 0);
                if (loggedSoftwareRoute_[index] != route) {
                    loggedSoftwareRoute_[index] = route;
                    const char* name = route == 2 ? "NV12 VP / TrueHDR request accepted" :
                        route == 1 ? (hdrOutput_ ? "NV12 VP SDR / explicit SDR-to-PQ (no RTX)"
                                                               : "NV12 VP / SDR") :
                        route == 3 ? "BGRA / SDR-to-PQ fallback (no RTX)" :
                        route == 4 ? "HDR-tagged BGRA / legacy software path (no RTX)" : "BGRA / SDR";
                    appendDiagnostic("Software presentation: pane=" + std::to_string(index + 1) +
                        " input=" + std::to_string(sourceWidth) + "x" + std::to_string(sourceHeight) +
                        " output=" + std::to_string(slots_[index].outputWidth) + "x" +
                        std::to_string(slots_[index].outputHeight) + " route=" + name);
                }
            }
        }
        const bool softwarePq = hdrOutput_ &&
            slots_[index].outputFormat == DXGI_FORMAT_B8G8R8A8_UNORM && !slots_[index].hdrSource;
        if (softwarePq && !prepareSoftwarePqTexture(index)) continue;
        // The fallback uses two offscreen passes; rebind the presentation
        // target and selected shader for this pane and every following pane.
        context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        context_->PSSetShader(softwarePq ? copyShader_.Get() : pixelShader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, pixelParamsBuffer_.GetAddressOf());
        context_->PSSetConstantBuffers(1, 1, smartVibranceBuffer_.GetAddressOf());
        const auto mode = views[index].mode;
        const RectF fitted = mode == ViewMode::Fit
            ? fitInside(cells[index], slots_[index].outputWidth, slots_[index].outputHeight)
            : cells[index];
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        if (mode == ViewMode::Fill) {
            const float sourceAspect = static_cast<float>(slots_[index].outputWidth) /
                                       std::max(1, slots_[index].outputHeight);
            const float cellAspect = cells[index].width / std::max(1.0F, cells[index].height);
            if (sourceAspect > cellAspect) scaleX = cellAspect / sourceAspect;
            else scaleY = sourceAspect / cellAspect;
        }
        const float zoom = std::clamp(views[index].zoom, 1.0F, 4.0F);
        scaleX /= zoom;
        scaleY /= zoom;
        const float transform[] = {scaleX, scaleY, (1.0F - scaleX) * 0.5F,
                                   (1.0F - scaleY) * 0.5F};
        context_->UpdateSubresource(uvTransformBuffer_.Get(), 0, nullptr, transform, 0, 0);
        const float width = static_cast<float>(std::max(1, slots_[index].outputWidth));
        const float height = static_cast<float>(std::max(1, slots_[index].outputHeight));
        const float clock = static_cast<float>(std::fmod(std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count(), 3600.0));
        const float params[] = {
            width, height, static_cast<float>(frame->serial), clock,
            1.0F / width, 1.0F / height, 0.0F, 0.0F,
            // Safe c2/pPrev initial state. True temporal feedback would require
            // a separate history texture or a player-specific analysis pass.
            0.0F, 0.0F, 0.0F, 0.0F};
        context_->UpdateSubresource(pixelParamsBuffer_.Get(), 0, nullptr, params, 0, 0);
        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = fitted.x + 1.0F;
        viewport.TopLeftY = fitted.y + 1.0F;
        viewport.Width = std::max(1.0F, fitted.width - 2.0F);
        viewport.Height = std::max(1.0F, fitted.height - 2.0F);
        viewport.MinDepth = 0.0F;
        viewport.MaxDepth = 1.0F;
        context_->RSSetViewports(1, &viewport);
        ID3D11ShaderResourceView* view = softwarePq ? softwarePq_[index].pqView.Get()
                                                  : slots_[index].view.Get();
        context_->PSSetShaderResources(0, 1, &view);
        bindVibranceColorSpace(index);
        context_->Draw(4, 0);
        ID3D11ShaderResourceView* empty = nullptr;
        context_->PSSetShaderResources(0, 1, &empty);
    }
    if (overlay_ && hdrOutput_) {
        // Direct2D cannot draw sRGB colours straight into a PQ back buffer:
        // white would come out at 10,000 nits. It paints a layer instead and
        // a shader pass brings that in at the display's SDR white.
        compositeOverlayLayer();
    } else if (overlay_) {
        // Direct2D draws through the same immediate context, so the painter
        // runs inside the lock too. In a flip-model chain buffer 0 is always
        // the buffer about to be presented.
        Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
        if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            overlay_(backBuffer.Get(), false);
            // D2D leaves its own pipeline state behind; the next frame
            // rebinds everything it needs, but the render target must be ours.
            context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        }
    }
    const auto ms = [](auto from, auto to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    // The renderer and every D3D11VA decoder share the immediate context, so
    // DXGI presentation stays in this critical section: Present overlapping
    // a decoder's context work can hang the whole device. But Present blocks
    // while the GPU is still on this frame -- RTX Video HDR on a 4K60 stream
    // costs it some 14 ms of every 16.7 -- and inside the lock that starved
    // the decoders to below the frame rate and made a seek's preroll crawl.
    // So the frame's GPU work is waited for first, outside the lock, on an
    // event query polled with the lock taken only for each poll; Present
    // then has nothing left to absorb but the blank, which the caller has
    // already waited for on the frame-latency object.
    const auto drawFinished = PerfClock::now();
    const double heldBeforeWait = ms(lockAcquired, drawFinished);
    double gpuWaitMs = 0.0;
    if (frameQuery_) {
        context_->End(frameQuery_.Get());
        context_->Flush();
        deviceLock.unlock();
        const auto waitStarted = PerfClock::now();
        for (;;) {
            HRESULT ready = S_OK;
            {
                std::scoped_lock poll(deviceMutex_);
                ready = context_->GetData(frameQuery_.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH);
            }
            // S_FALSE is "not yet"; anything else is done, or a device that
            // is gone and will say so at Present.
            if (ready != S_FALSE) break;
            if (ms(waitStarted, PerfClock::now()) > 250.0) break;
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        gpuWaitMs = ms(waitStarted, PerfClock::now());
        deviceLock.lock();
    }
    const auto presentStarted = PerfClock::now();
    const HRESULT hr = swapChain_->Present(1, 0);
    const auto presentFinished = PerfClock::now();
    if (FAILED(hr)) {
        noteFailure("Present", hr);
    }
    ++stats_.frames;
    stats_.lockWaitMs += ms(lockRequested, lockAcquired);
    stats_.convertMs += convertMs;
    stats_.presentMs += ms(presentStarted, presentFinished);
    stats_.gpuWaitMs += gpuWaitMs;
    stats_.lockHeldMs += heldBeforeWait + ms(presentStarted, presentFinished);
    // Whatever was inside the lock before the wait is the draw work itself.
    stats_.drawMs += heldBeforeWait - convertMs;
}

RenderStats D3DRenderer::takeStats() {
    std::scoped_lock lock(deviceMutex_);
    const RenderStats snapshot = stats_;
    stats_ = RenderStats{};
    return snapshot;
}

void D3DRenderer::setError(const char* operation, HRESULT hr) {
    std::ostringstream out;
    out << operation << " failed: 0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr);
    error_ = out.str();
    appendDiagnostic("GPU ERROR: " + error_);
}

}  // namespace quaddeck
