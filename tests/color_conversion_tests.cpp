#include "ColorConversion.hpp"
#include "D3DRenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace quaddeck;
using Microsoft::WRL::ComPtr;

namespace quaddeck {
struct D3DRendererColorProbe {
    struct Readback {
        UINT width{};
        UINT height{};
        DXGI_FORMAT format{};
        std::vector<std::uint8_t> pixels;
    };

    struct SoftwareResult {
        Readback readback;
        DXGI_FORMAT inputFormat{};
        int inputWidth{};
        int inputHeight{};
        bool extensionAccepted{};
    };

    static void checked(D3DRenderer& renderer, HRESULT result, const char* operation) {
        if (FAILED(result)) {
            renderer.setError(operation, result);
            throw std::runtime_error(renderer.error());
        }
    }

    // The colour tests only need textures and the video context. Neither the
    // default suite nor the optional RTX probe creates a window or swap chain.
    static void initializeOffscreen(D3DRenderer& renderer) {
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL actual{};
        checked(renderer, D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
            levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
            &renderer.device_, &actual, &renderer.context_), "Create offscreen colour device");
        checked(renderer, renderer.device_.As(&renderer.videoDevice_), "Query colour video device");
        checked(renderer, renderer.context_.As(&renderer.videoContext_), "Query colour video context");
        if (FAILED(renderer.context_.As(&renderer.videoContext1_))) renderer.videoContext1_.Reset();
        ComPtr<IDXGIDevice> dxgiDevice;
        checked(renderer, renderer.device_.As(&dxgiDevice), "Query colour DXGI device");
        checked(renderer, dxgiDevice->GetAdapter(&renderer.adapter_), "Get colour adapter");
        DXGI_ADAPTER_DESC adapter{};
        checked(renderer, renderer.adapter_->GetDesc(&adapter), "Describe colour adapter");
        renderer.enhancementStatus_.adapterName = adapter.Description;
        renderer.enhancementStatus_.nvidiaAdapter = adapter.VendorId == 0x10DE;
        if (!renderer.createPipeline()) throw std::runtime_error(renderer.error());
    }

    static bool probeRtx(D3DRenderer& renderer) {
        renderer.probeVideoEnhancements();
        return renderer.videoContext1_ && renderer.enhancementStatus_.nvidiaAdapter &&
               renderer.enhancementStatus_.rtxHdrSupported;
    }

    static void pqOutput(D3DRenderer& renderer, bool requestRtx) {
        renderer.setVideoEnhancements({false, requestRtx});
        renderer.slots_ = {};
        // Explicit offscreen output request: no display/swap-chain eligibility
        // is inferred, and extension acceptance still comes from the driver.
        renderer.hdrOutput_ = true;
        renderer.enhancementStatus_.sdrWhiteNits = 288.0F;
        if (!renderer.setShaderPreset(ShaderPreset::Normal))
            throw std::runtime_error(renderer.error());
    }

    static Readback readback(D3DRenderer& renderer, ID3D11Texture2D* texture = nullptr) {
        if (!texture) texture = renderer.slots_[0].texture.Get();
        if (!texture) throw std::runtime_error("Colour conversion produced no texture");
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
            description.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
            description.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
            throw std::runtime_error("Unexpected colour readback format");
        Readback result{description.Width, description.Height, description.Format, {}};
        description.Usage = D3D11_USAGE_STAGING;
        description.BindFlags = 0;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        checked(renderer, renderer.device_->CreateTexture2D(&description, nullptr, &staging),
                "Create colour readback texture");
        renderer.context_->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(renderer, renderer.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                "Map colour readback texture");
        const std::size_t bytesPerPixel = result.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
        const std::size_t stride = static_cast<std::size_t>(result.width) * bytesPerPixel;
        result.pixels.resize(stride * result.height);
        for (UINT y = 0; y < result.height; ++y) {
            std::copy_n(static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch,
                        stride, result.pixels.data() + y * stride);
        }
        renderer.context_->Unmap(staging.Get(), 0);
        return result;
    }

    static std::vector<std::uint8_t> convert(D3DRenderer& renderer,
                                            const HardwareVideoFrame& frame) {
        std::scoped_lock lock(renderer.deviceMutex_);
        if (!renderer.convertSurface(0, frame, {frame.width, frame.height})) {
            throw std::runtime_error(renderer.error());
        }
        return readback(renderer).pixels;
    }

    static SoftwareResult convertSoftware(D3DRenderer& renderer, const HardwareVideoFrame& frame,
                                          const SurfaceSize& requested, bool prepareSdrPq = false) {
        std::scoped_lock lock(renderer.deviceMutex_);
        if (!renderer.convertSoftwareSurface(0, frame, requested))
            throw std::runtime_error("Software video-processor conversion failed: " + renderer.error());
        const auto& slot = renderer.slots_[0];
        ID3D11Texture2D* presentation = slot.texture.Get();
        if (prepareSdrPq) {
            if (slot.outputFormat != DXGI_FORMAT_B8G8R8A8_UNORM)
                throw std::runtime_error("RTX-off processor output must use SDR BGRA");
            if (!renderer.prepareSoftwarePqTexture(0))
                throw std::runtime_error("SDR processor PQ presentation failed: " + renderer.error());
            presentation = renderer.softwarePq_[0].pq.Get();
            if (!presentation) throw std::runtime_error("SDR processor produced no PQ presentation texture");
        }
        return {readback(renderer, presentation), slot.inputFormat, slot.inputWidth, slot.inputHeight,
                slot.rtxHdrApplied};
    }
    static ID3D11VideoProcessor* processor(D3DRenderer& renderer) {
        return renderer.slots_[0].processor.Get();
    }
};
}  // namespace quaddeck

namespace {
constexpr int width = 64;
constexpr int height = 32;
struct Yuv { int y, u, v; };
constexpr std::array<Yuv, 8> patches{{
    {16, 128, 128}, {235, 128, 128}, {0, 128, 128}, {255, 128, 128},
    {100, 90, 200}, {145, 54, 34}, {41, 240, 110}, {128, 160, 96}}};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Independent textbook inverse Y'CbCr equations. Production delegates to
// libswscale or the video processor and does not implement this arithmetic.
std::array<int, 3> expectedRgb(Yuv code, AVColorSpace matrix, bool full) {
    const double kr = matrix == AVCOL_SPC_BT709 ? 0.2126 : 0.299;
    const double kb = matrix == AVCOL_SPC_BT709 ? 0.0722 : 0.114;
    const double y = (code.y - (full ? 0.0 : 16.0)) / (full ? 255.0 : 219.0);
    const double cb = (code.u - 128.0) / (full ? 255.0 : 224.0);
    const double cr = (code.v - 128.0) / (full ? 255.0 : 224.0);
    const double r = y + 2.0 * (1.0 - kr) * cr;
    const double b = y + 2.0 * (1.0 - kb) * cb;
    const double g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    const auto byte = [](double value) {
        return static_cast<int>(std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
    };
    return {byte(r), byte(g), byte(b)};
}

void checkPixels(const std::vector<std::uint8_t>& pixels, AVColorSpace matrix,
                 bool full, const char* path, int tolerance = 3) {
    int largestError = 0;
    for (std::size_t patch = 0; patch < patches.size(); ++patch) {
        const auto expected = expectedRgb(patches[patch], matrix, full);
        const std::size_t offset = (height / 2 * width + patch * 8 + 4) * 4;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int actual = pixels[offset + 2 - channel];
            const int error = std::abs(actual - expected[channel]);
            largestError = std::max(largestError, error);
            if (error > tolerance) {
                throw std::runtime_error(std::string(path) + " matrix=" +
                    std::to_string(matrix) + " full=" + std::to_string(full) +
                    " patch=" + std::to_string(patch) + " channel=" +
                    std::to_string(channel) + " expected=" + std::to_string(expected[channel]) +
                    " actual=" + std::to_string(actual));
            }
        }
    }
    std::cout << path << " source-matrix=" << matrix
              << ((matrix == AVCOL_SPC_UNSPECIFIED || matrix == AVCOL_SPC_BT2020_NCL)
                    ? " (SD fallback=601)" : "") << " full=" << full
              << " max-channel-error=" << largestError << " PASS\n";
}

void metadataPolicy() {
    AVFrame frame{};
    frame.format = AV_PIX_FMT_YUV420P;
    frame.colorspace = AVCOL_SPC_UNSPECIFIED;
    require(videoColorDescription(&frame, 720, 576).matrix == VideoColorMatrix::Bt601,
            "Untagged SD must default to 601");
    require(videoColorDescription(&frame, 1920, 1080).matrix == VideoColorMatrix::Bt709,
            "Untagged HD must default to 709");
    require(!videoColorDescription(&frame, 1920, 1080).fullRange,
            "Untagged YUV must default to limited");
    require(videoColorDescription(nullptr, 1920, 1080).matrix == VideoColorMatrix::Bt709,
            "Missing hardware owner metadata must use original dimensions");
    frame.colorspace = AVCOL_SPC_BT470BG;
    require(videoColorDescription(&frame, 1920, 1080).matrix == VideoColorMatrix::Bt601,
            "Explicit 601 must override HD fallback");
    frame.colorspace = AVCOL_SPC_BT2020_NCL;
    require(videoColorDescription(&frame, 1920, 1080).matrix == VideoColorMatrix::Bt709,
            "Unsupported 2020 NCL matrix must use documented fallback");
    frame.colorspace = AVCOL_SPC_BT2020_CL;
    require(videoColorDescription(&frame, 1920, 1080).matrix == VideoColorMatrix::Bt709,
            "Unsupported constant-luminance matrix must use documented fallback");
    frame.format = AV_PIX_FMT_YUVJ420P;
    require(videoColorDescription(&frame, width, height).fullRange,
            "Legacy JPEG YUV must default to full");
    frame.format = AV_PIX_FMT_BGRA;
    require(videoColorDescription(&frame, width, height).fullRange,
            "Untagged RGB must default to full");
    frame.color_range = AVCOL_RANGE_MPEG;
    require(!videoColorDescription(&frame, width, height).fullRange,
            "Explicit range must override the pixel-format fallback");
}

void softwarePixels() {
    AVFrame* frame = av_frame_alloc();
    require(frame != nullptr, "Cannot allocate software test frame");
    frame->width = width;
    frame->height = height;
    frame->format = AV_PIX_FMT_YUV420P;
    require(av_frame_get_buffer(frame, 32) >= 0, "Cannot allocate YUV test planes");
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            frame->data[0][y * frame->linesize[0] + x] = static_cast<std::uint8_t>(patches[x / 8].y);
    for (int y = 0; y < height / 2; ++y) {
        for (int x = 0; x < width / 2; ++x) {
            frame->data[1][y * frame->linesize[1] + x] = static_cast<std::uint8_t>(patches[x / 4].u);
            frame->data[2][y * frame->linesize[2] + x] = static_cast<std::uint8_t>(patches[x / 4].v);
        }
    }
    SwsContext* converter = nullptr;
    std::vector<std::uint8_t> pixels(width * height * 4);
    std::uint8_t* destination[] = {pixels.data(), nullptr, nullptr, nullptr};
    int strides[] = {width * 4, 0, 0, 0};
    // Reuse one converter while changing metadata and then returning to 601.
    for (const auto matrix : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709,
                              AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT2020_NCL, AVCOL_SPC_SMPTE170M}) {
        for (bool full : {false, true}) {
            frame->colorspace = matrix;
            frame->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            auto* retainedConverter = converter;
            converter = sws_getCachedContext(converter, width, height, AV_PIX_FMT_YUV420P,
                width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            require(converter != nullptr, "Cannot create swscale test converter");
            require(!retainedConverter || converter == retainedConverter,
                    "Test must reuse the same SwsContext across metadata changes");
            require(configureSoftwareColorspace(converter, *frame) >= 0,
                    "Cannot configure swscale test colours");
            require(sws_scale(converter, frame->data, frame->linesize, 0, height,
                              destination, strides) == height, "Cannot convert software pixels");
            checkPixels(pixels, matrix, full, "swscale");
        }
    }
    sws_freeContext(converter);
    av_frame_free(&frame);

    AVFrame rgb{};
    rgb.width = width;
    rgb.height = height;
    rgb.format = AV_PIX_FMT_BGRA;
    converter = sws_getContext(width, height, AV_PIX_FMT_BGRA,
        width, height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    require(converter != nullptr, "Cannot create RGB test converter");
    require(configureSoftwareColorspace(converter, rgb) >= 0,
            "Colour metadata configuration must still accept RGB input");
    std::vector<std::uint8_t> rgbSource(width * height * 4, 85);
    const std::uint8_t* source[] = {rgbSource.data(), nullptr, nullptr, nullptr};
    require(sws_scale(converter, source, strides, 0, height, destination, strides) == height,
            "Cannot convert RGB input");
    require(pixels == rgbSource, "Untagged RGB must preserve its full-range codes");
    sws_freeContext(converter);
    std::cout << "RGB passthrough PASS\n";
}

constexpr std::array<int, 8> grayCodes{{16, 32, 64, 96, 128, 160, 192, 235}};

void updateSoftwareFallback(HardwareVideoFrame& frame) {
    const auto& source = *frame.ownerFrame;
    frame.stride = frame.width * 4;
    frame.bgra.resize(static_cast<std::size_t>(frame.stride) * frame.height);
    SwsContext* converter = sws_getContext(source.width, source.height,
        static_cast<AVPixelFormat>(source.format), frame.width, frame.height,
        AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    require(converter != nullptr, "Cannot create synthetic software fallback converter");
    const int configured = configureSoftwareColorspace(converter, source);
    std::uint8_t* destination[] = {frame.bgra.data(), nullptr, nullptr, nullptr};
    const int strides[] = {frame.stride, 0, 0, 0};
    const int rows = configured >= 0
        ? sws_scale(converter, source.data, source.linesize, 0, source.height, destination, strides)
        : -1;
    sws_freeContext(converter);
    require(rows == frame.height, "Cannot generate synthetic software BGRA fallback");
}

std::unique_ptr<HardwareVideoFrame> syntheticSoftwareFrame(int inputWidth, int inputHeight,
                                                         bool gray = false) {
    auto frame = std::make_unique<HardwareVideoFrame>();
    frame->width = inputWidth;
    frame->height = inputHeight;
    frame->hardware = false;
    frame->ownerFrame = av_frame_alloc();
    require(frame->ownerFrame != nullptr, "Cannot allocate retained software frame");
    auto& source = *frame->ownerFrame;
    source.width = inputWidth;
    source.height = inputHeight;
    source.format = AV_PIX_FMT_YUV420P;
    source.colorspace = AVCOL_SPC_BT709;
    source.color_range = AVCOL_RANGE_MPEG;
    source.color_primaries = AVCOL_PRI_BT709;
    source.color_trc = AVCOL_TRC_BT709;
    require(av_frame_get_buffer(&source, 32) >= 0, "Cannot allocate retained software planes");
    for (int y = 0; y < inputHeight; ++y) {
        for (int x = 0; x < inputWidth; ++x) {
            const auto patch = static_cast<std::size_t>(x * 8 / inputWidth);
            source.data[0][y * source.linesize[0] + x] =
                static_cast<std::uint8_t>(gray ? grayCodes[patch] : patches[patch].y);
        }
    }
    for (int y = 0; y < inputHeight / 2; ++y) {
        for (int x = 0; x < inputWidth / 2; ++x) {
            const auto patch = static_cast<std::size_t>(x * 16 / inputWidth);
            source.data[1][y * source.linesize[1] + x] =
                static_cast<std::uint8_t>(gray ? 128 : patches[patch].u);
            source.data[2][y * source.linesize[2] + x] =
                static_cast<std::uint8_t>(gray ? 128 : patches[patch].v);
        }
    }
    updateSoftwareFallback(*frame);
    return frame;
}

void softwareBridgePixels(D3DRenderer& renderer) {
    auto frame = syntheticSoftwareFrame(width, height);
    ID3D11VideoProcessor* retainedProcessor = nullptr;
    for (const auto matrix : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709, AVCOL_SPC_SMPTE170M}) {
        for (bool full : {false, true}) {
            frame->ownerFrame->colorspace = matrix;
            frame->ownerFrame->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            updateSoftwareFallback(*frame);
            ++frame->serial;
            const auto result = D3DRendererColorProbe::convertSoftware(renderer, *frame, {width, height});
            require(result.inputFormat == DXGI_FORMAT_NV12, "Software bridge must upload NV12");
            require(result.readback.format == DXGI_FORMAT_B8G8R8A8_UNORM,
                    "SDR software bridge must produce BGRA");
            require(result.inputWidth == width && result.inputHeight == height &&
                    result.readback.width == width && result.readback.height == height,
                    "Software bridge changed synthetic geometry");
            require(!result.extensionAccepted, "Default software bridge must not request TrueHDR");
            if (!retainedProcessor) retainedProcessor = D3DRendererColorProbe::processor(renderer);
            require(retainedProcessor == D3DRendererColorProbe::processor(renderer),
                    "Software metadata change unnecessarily recreated the video processor");
            checkPixels(result.readback.pixels, matrix, full, "software NV12 video processor");
        }
    }
    // Retained decoder planes determine the bridge's input geometry even when
    // the existing BGRA fallback was scaled for a smaller pane.
    frame->width = width / 2;
    frame->height = height / 2;
    updateSoftwareFallback(*frame);
    ++frame->serial;
    const auto result = D3DRendererColorProbe::convertSoftware(renderer, *frame, {width, height});
    require(result.inputWidth == width && result.inputHeight == height &&
            result.readback.width == width && result.readback.height == height,
            "Software bridge used downscaled BGRA dimensions instead of retained source dimensions");
    checkPixels(result.readback.pixels, AVCOL_SPC_SMPTE170M, true,
                "software NV12 retained original dimensions");

    // Read the production upload counter rather than assuming a reused output
    // processor means the retained decoder planes were packed only once.
    require(renderer.takeStats().softwareNv12Uploads > 0,
            "Software bridge did not report its NV12 uploads");
    D3DRendererColorProbe::convertSoftware(renderer, *frame, {width, height});
    require(renderer.takeStats().softwareNv12Uploads == 0,
            "Converting the same frame serial uploaded NV12 again");
    const auto resized = D3DRendererColorProbe::convertSoftware(renderer, *frame,
                                                              {width / 2, height / 2});
    require(resized.readback.width == width / 2 && resized.readback.height == height / 2,
            "Same-frame software bridge did not resize its output");
    require(renderer.takeStats().softwareNv12Uploads == 0,
            "Resizing the output reuploaded the same retained software frame");
    ++frame->serial;
    D3DRendererColorProbe::convertSoftware(renderer, *frame, {width / 2, height / 2});
    require(renderer.takeStats().softwareNv12Uploads == 1,
            "A new software frame serial must cause exactly one NV12 upload");
    std::cout << "Software NV12 same-frame upload reuse and output resize PASS\n";
}

// Independent IEEE 754 binary16 decoder; readback does not use a production
// conversion helper or reinterpret half-float bits as an integer colour code.
double halfValue(std::uint16_t bits) {
    const unsigned exponent = (bits >> 10) & 31U;
    const unsigned mantissa = bits & 1023U;
    double value{};
    if (exponent == 0) value = std::ldexp(static_cast<double>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? std::numeric_limits<double>::quiet_NaN()
                                             : std::numeric_limits<double>::infinity();
    else value = std::ldexp(static_cast<double>(1024U + mantissa), static_cast<int>(exponent) - 25);
    return bits & 0x8000U ? -value : value;
}

double pqNits(double code) {
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0, c2 = 2413.0 / 128.0, c3 = 2392.0 / 128.0;
    const double p = std::pow(code / 1023.0, 1.0 / m2);
    return 10000.0 * std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}

struct PqObservation {
    std::array<std::array<double, 3>, 8> codes{};
    std::array<std::array<double, 3>, 8> nits{};
};

PqObservation observePq(const D3DRendererColorProbe::SoftwareResult& result,
                        int inputWidth, int inputHeight, bool requestRtx) {
    const auto& image = result.readback;
    require(result.inputFormat == DXGI_FORMAT_NV12 && result.inputWidth == inputWidth &&
            result.inputHeight == inputHeight, "RTX probe did not use retained software NV12 input");
    const bool half = image.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    require(requestRtx ? image.format == DXGI_FORMAT_R10G10B10A2_UNORM : half,
            "RTX probe output does not match the requested production presentation path");
    require(result.extensionAccepted == requestRtx,
            "RTX probe stream extension acceptance differs from the request");
    const std::size_t bytesPerPixel = half ? 8 : 4;
    bool nonBlack = false;
    for (std::size_t offset = 0; offset < image.pixels.size(); offset += bytesPerPixel) {
        if (half) {
            for (unsigned channel = 0; channel < 3; ++channel) {
                std::uint16_t bits{};
                std::memcpy(&bits, image.pixels.data() + offset + channel * 2, sizeof(bits));
                require((bits & 0x7c00U) != 0x7c00U,
                        "SDR PQ presentation contains a nonfinite half-float pixel");
                nonBlack = nonBlack || (bits & 0x7fffU) != 0;
            }
        } else {
            std::uint32_t packed{};
            std::memcpy(&packed, image.pixels.data() + offset, sizeof(packed));
            nonBlack = nonBlack || (packed & 0x3fffffffU) != 0;
        }
    }
    require(nonBlack, "RTX probe PQ output is entirely black");
    std::cout << "Software RTX probe input=" << inputWidth << 'x' << inputHeight
              << " output=" << image.width << 'x' << image.height
              << " upload=NV12 output-format=" << (half ? "R16G16B16A16_FLOAT" : "R10G10B10A2")
              << " request=" << requestRtx
              << " extension-accepted=" << result.extensionAccepted << '\n';
    PqObservation observation;
    for (std::size_t patch = 0; patch < grayCodes.size(); ++patch) {
        const std::size_t x = (patch * 2 + 1) * image.width / 16;
        const std::size_t offset =
            (image.height / 2 * static_cast<std::size_t>(image.width) + x) * bytesPerPixel;
        std::uint32_t packed{};
        if (!half) std::memcpy(&packed, image.pixels.data() + offset, sizeof(packed));
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (half) {
                std::uint16_t bits{};
                std::memcpy(&bits, image.pixels.data() + offset + channel * 2, sizeof(bits));
                const double code = halfValue(bits);
                require(std::isfinite(code) && code >= 0 && code <= 1,
                        "SDR PQ presentation contains an invalid PQ code");
                observation.codes[patch][channel] = code * 1023.0;
            } else {
                observation.codes[patch][channel] = (packed >> (channel * 10)) & 1023U;
            }
            const double nits = pqNits(observation.codes[patch][channel]);
            require(std::isfinite(nits) && nits >= 0 && nits <= 10000.001,
                    "RTX probe produced invalid decoded PQ luminance");
            observation.nits[patch][channel] = nits;
            if (!requestRtx) {
                // Neutral BT.709 Y'CbCr maps to neutral RGB; the independent
                // reference accounts for limited range and the G22 transfer.
                const double gray = std::clamp((grayCodes[patch] - 16.0) / 219.0, 0.0, 1.0);
                const double expected = 288.0 * std::pow(gray, 2.2);
                require(std::abs(nits - expected) <= 3.0,
                        "RTX-off presentation gray does not match G22 at 288-nit SDR white");
            }
        }
        std::cout << "  Y=" << grayCodes[patch] << " PQ10-equivalent="
                  << observation.codes[patch][0] << ',' << observation.codes[patch][1] << ','
                  << observation.codes[patch][2] << " nits=" << observation.nits[patch][0] << ','
                  << observation.nits[patch][1] << ',' << observation.nits[patch][2] << '\n';
    }
    return observation;
}

void softwareRtxProbe() {
    D3DRenderer renderer;
    D3DRendererColorProbe::initializeOffscreen(renderer);
    const bool available = D3DRendererColorProbe::probeRtx(renderer);
    const auto status = renderer.enhancementStatus();
    std::wcout << L"Software RTX probe adapter=" << status.adapterName << L'\n';
    if (!available) {
        std::cout << "Software RTX probe SKIP: NVIDIA TrueHDR capability and VideoContext1 required\n";
        return;
    }
    struct Geometry { int inputWidth, inputHeight, outputWidth, outputHeight; };
    for (const auto geometry : {Geometry{4320,2160,4320,2160}, Geometry{4320,2160,3840,1920},
                               Geometry{1920,1080,1920,1080}}) {
        auto frame = syntheticSoftwareFrame(geometry.inputWidth, geometry.inputHeight, true);
        std::array<PqObservation, 2> observations;
        for (int request = 0; request < 2; ++request) {
            D3DRendererColorProbe::pqOutput(renderer, request != 0);
            ++frame->serial;
            const auto result = D3DRendererColorProbe::convertSoftware(renderer, *frame,
                {geometry.outputWidth, geometry.outputHeight}, request == 0);
            require(result.readback.width == static_cast<UINT>(geometry.outputWidth) &&
                    result.readback.height == static_cast<UINT>(geometry.outputHeight),
                    "RTX probe output geometry differs from request");
            observations[request] = observePq(result, geometry.inputWidth, geometry.inputHeight, request != 0);
        }
        double largestCodeDifference = 0;
        double largestNitsDifference = 0;
        for (std::size_t patch = 0; patch < grayCodes.size(); ++patch) {
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const double difference = std::abs(observations[1].codes[patch][channel] -
                                                   observations[0].codes[patch][channel]);
                largestCodeDifference = std::max(largestCodeDifference, difference);
                largestNitsDifference = std::max(largestNitsDifference, std::abs(
                    observations[1].nits[patch][channel] - observations[0].nits[patch][channel]));
            }
        }
        std::cout << "RTX off/on max-PQ10-equivalent-difference=" << largestCodeDifference
                  << " max-nits-difference=" << largestNitsDifference << '\n';
    }
    std::cout << "Software RTX probe conversion/readback PASS; extension acceptance and synthetic\n"
                 "pixel differences do not establish AI inference or display appearance.\n";
}

void hardwarePixels(D3DRenderer& renderer, DXGI_FORMAT format) {
    const bool tenBit = format == DXGI_FORMAT_P010;
    const int pitch = width * (tenBit ? 2 : 1);
    std::vector<std::uint8_t> planes(pitch * height * 3 / 2);
    auto setCode = [&](int row, int x, int value) {
        auto* pixel = planes.data() + row * pitch + x * (tenBit ? 2 : 1);
        // P010 stores a 10-bit code in the most significant ten bits.
        if (tenBit) { pixel[0] = 0; pixel[1] = static_cast<std::uint8_t>(value); }
        else pixel[0] = static_cast<std::uint8_t>(value);
    };
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) setCode(y, x, patches[x / 8].y);
    for (int y = 0; y < height / 2; ++y)
        for (int x = 0; x < width; x += 2) {
            setCode(height + y, x, patches[x / 8].u);
            setCode(height + y, x + 1, patches[x / 8].v);
        }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = format;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_DECODER;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = planes.data();
    data.SysMemPitch = pitch;
    ComPtr<ID3D11Texture2D> texture;
    require(SUCCEEDED(renderer.device()->CreateTexture2D(&description, &data, &texture)),
            "Cannot create YUV hardware input surface");
    HardwareVideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.hardware = true;
    frame.texture = texture.Get();
    frame.ownerFrame = av_frame_alloc();
    require(frame.ownerFrame != nullptr, "Cannot create hardware colour metadata");
    frame.ownerFrame->format = AV_PIX_FMT_D3D11;
    ID3D11VideoProcessor* retainedProcessor = nullptr;
    for (const auto matrix : {AVCOL_SPC_SMPTE170M, AVCOL_SPC_BT709,
                              AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT2020_NCL, AVCOL_SPC_SMPTE170M}) {
        for (bool full : {false, true}) {
            frame.ownerFrame->colorspace = matrix;
            frame.ownerFrame->color_range = full ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            ++frame.serial;
            const auto pixels = D3DRendererColorProbe::convert(renderer, frame);
            if (!retainedProcessor) retainedProcessor = D3DRendererColorProbe::processor(renderer);
            require(retainedProcessor == D3DRendererColorProbe::processor(renderer),
                    "Metadata change unnecessarily recreated the video processor");
            checkPixels(pixels, matrix, full, tenBit ? "P010" : "NV12");
        }
    }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--software-rtx-probe") {
            softwareRtxProbe();
            return 0;
        }
        require(argc == 1, "Usage: QuadDeckColorConversionTests [--software-rtx-probe]");
        metadataPolicy();
        softwarePixels();
        {
            D3DRenderer renderer;
            D3DRendererColorProbe::initializeOffscreen(renderer);
            hardwarePixels(renderer, DXGI_FORMAT_NV12);
            hardwarePixels(renderer, DXGI_FORMAT_P010);
            softwareBridgePixels(renderer);
        }
        std::cout << "Colour conversion tests PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Colour conversion tests FAILED: " << error.what() << '\n';
        return 1;
    }
}
