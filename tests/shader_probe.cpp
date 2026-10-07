#include "D3DRenderer.hpp"
#include "ColorConversion.hpp"
#include "D3D11TestSupport.hpp"

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace quaddeck;

namespace quaddeck {
// Exercise the production shader and per-pane binding on offscreen float
// textures. These checks need D3D11, not an HDR display or NVIDIA TrueHDR.
// The supplied PQ pixels stand in for processor output; they do not test AI.
struct D3DRendererShaderProbe {
    using Pixel = std::array<float, 4>;
    enum class Route { Sdr, RtxPq, NativePq, RefusedPq, Software };

    static void require(bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    }

    static std::vector<Pixel> draw(D3DRenderer& r, const std::vector<Pixel>& pixels,
                                   Route route, SmartVibranceSettings settings = {}) {
        using Microsoft::WRL::ComPtr;
        std::scoped_lock lock(r.deviceMutex_);
        r.setSmartVibranceSettings(settings);
        const UINT width = static_cast<UINT>(std::bit_ceil(pixels.size()));
        std::vector<Pixel> padded(width);
        std::copy(pixels.begin(), pixels.end(), padded.begin());
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{};
        data.pSysMem = padded.data();
        data.SysMemPitch = width * sizeof(Pixel);
        ComPtr<ID3D11Texture2D> input, output, staging;
        require(SUCCEEDED(r.device_->CreateTexture2D(&desc, &data, &input)), "HDR input texture");
        ComPtr<ID3D11ShaderResourceView> srv;
        require(SUCCEEDED(r.device_->CreateShaderResourceView(input.Get(), nullptr, &srv)), "HDR input SRV");
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        require(SUCCEEDED(r.device_->CreateTexture2D(&desc, nullptr, &output)), "HDR output texture");
        ComPtr<ID3D11RenderTargetView> rtv;
        require(SUCCEEDED(r.device_->CreateRenderTargetView(output.Get(), nullptr, &rtv)), "HDR output RTV");
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(r.device_->CreateTexture2D(&desc, nullptr, &staging)), "HDR readback texture");

        auto* context = r.context_.Get();
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = viewport.MaxDepth = 1.0F;
        context->RSSetViewports(1, &viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(r.vertexShader_.Get(), nullptr, 0);
        const float identity[] = {1, 1, 0, 0};
        context->UpdateSubresource(r.uvTransformBuffer_.Get(), 0, nullptr, identity, 0, 0);
        context->VSSetConstantBuffers(0, 1, r.uvTransformBuffer_.GetAddressOf());
        context->PSSetShader(r.pixelShader_.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, r.sampler_.GetAddressOf());
        context->PSSetConstantBuffers(1, 1, r.smartVibranceBuffer_.GetAddressOf());
        context->PSSetShaderResources(0, 1, srv.GetAddressOf());

        const bool previousHdr = r.hdrOutput_;
        r.hdrOutput_ = route != Route::Sdr;
        auto& slot = r.slots_[0];
        slot.hardware = route != Route::Software;
        slot.outputFormat = r.hdrOutput_ && slot.hardware ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                         : DXGI_FORMAT_B8G8R8A8_UNORM;
        slot.hdrSource = route == Route::NativePq;
        slot.rtxHdrApplied = route == Route::RtxPq;
        // Deliberately disagree: routing must not follow the last processor.
        r.enhancementStatus_.rtxHdrApplied = !slot.rtxHdrApplied;
        r.bindVibranceColorSpace(0);
        context->Draw(4, 0);
        r.hdrOutput_ = previousHdr;
        r.slots_[0] = {};
        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "HDR readback map");
        std::vector<Pixel> result(pixels.size());
        std::copy_n(static_cast<const Pixel*>(mapped.pData), result.size(), result.begin());
        context->Unmap(staging.Get(), 0);
        return result;
    }

    // Independent double-precision ST 2084 reference for input generation and
    // measuring linear luminance, not a CPU copy of the enhancement algorithm.
    static double pq(double nits) {
        const double p = std::pow(nits / 10000.0, 2610.0 / 16384.0);
        return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
                       (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
    }
    static double nits(double code) {
        const double p = std::pow(std::max(0.0, code), 32.0 / 2523.0);
        return 10000.0 * std::pow(std::max(0.0, p - 3424.0 / 4096.0) /
            (2413.0 / 128.0 - 2392.0 / 128.0 * p), 16384.0 / 2610.0);
    }
    static Pixel encode(double red, double green, double blue) {
        return {static_cast<float>(pq(red)), static_cast<float>(pq(green)),
                static_cast<float>(pq(blue)), 0.75F};
    }
    static double luminance(const Pixel& p) {
        return 0.2627 * nits(p[0]) + 0.6780 * nits(p[1]) + 0.0593 * nits(p[2]);
    }
    static double chroma(const Pixel& p) {
        const double y = luminance(p);
        double squared = 0;
        for (int c = 0; c < 3; ++c) squared += std::pow(nits(p[c]) - y, 2);
        return std::sqrt(squared) / std::max(y, 0.0001);
    }
    static void close(const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                      double tolerance, const char* message) {
        require(a.size() == b.size(), "HDR probe size mismatch");
        for (std::size_t i = 0; i < a.size(); ++i)
            for (int c = 0; c < 4; ++c)
                require(std::abs(a[i][c] - b[i][c]) <= tolerance, message);
    }

    static void softwarePqFallback(D3DRenderer& r) {
        // Exercise the real BGRA upload and both production presentation
        // passes. These pixels contain no media and need no HDR monitor/AI.
        HardwareVideoFrame frame;
        frame.width = 16; frame.height = 2; frame.stride = 64; frame.serial = 4242;
        const std::array<std::array<std::uint8_t, 3>, 8> rgb{{
            {0,0,0}, {255,255,255}, {128,128,128}, {255,0,0},
            {0,255,0}, {0,0,255}, {64,128,192}, {192,64,128}}};
        frame.bgra.resize(frame.stride * frame.height);
        for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
            const auto& p = rgb[x / 2];
            const int offset = y * frame.stride + 4 * x;
            frame.bgra[offset] = p[2]; frame.bgra[offset+1] = p[1];
            frame.bgra[offset+2] = p[0]; frame.bgra[offset+3] = 255;
        }
        const bool previousHdr = r.hdrOutput_;
        const float previousWhite = r.enhancementStatus_.sdrWhiteNits;
        r.hdrOutput_ = true;
        require(r.uploadSoftwareFrame(0, frame), "Software fallback upload");
        const auto uploads = r.stats_.softwareFrameUploads;
        require(r.uploadSoftwareFrame(0, frame), "Cached software fallback upload");
        require(r.stats_.softwareFrameUploads == uploads,
                "Paused fallback reuploaded unchanged BGRA pixels");
        const auto half = [](std::uint16_t bits) {
            const int exponent = (bits >> 10) & 31;
            const double mantissa = bits & 1023;
            const double value = exponent == 0 ? std::ldexp(mantissa, -24)
                : exponent == 31 ? std::numeric_limits<double>::infinity()
                : std::ldexp(1.0 + mantissa / 1024.0, exponent - 15);
            return bits & 0x8000 ? -value : value;
        };
        for (const auto preset : {ShaderPreset::Normal, ShaderPreset::Invert,
                                  ShaderPreset::SmartVibrancePlus}) {
            require(r.setShaderPreset(preset), "Fallback effect compile");
            r.setSmartVibranceSettings({1.0F, 0.5F, 0.003F, 45.0F});
            for (float white : {80.0F, 288.0F, 1000.0F}) {
                r.enhancementStatus_.sdrWhiteNits = white;
                require(r.prepareSoftwarePqTexture(0), "Software fallback PQ presentation");
                auto* output = r.softwarePq_[0].pq.Get();
                D3D11_TEXTURE2D_DESC desc{};
                output->GetDesc(&desc);
                desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
                desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                require(SUCCEEDED(r.device_->CreateTexture2D(&desc, nullptr, &staging)), "Fallback readback texture");
                r.context_->CopyResource(staging.Get(), output);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                require(SUCCEEDED(r.context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "Fallback readback map");
                double maxCodeError = 0;
                bool valid = true;
                for (std::size_t i = 0; i < rgb.size(); ++i) {
                    std::array<double,3> linear{};
                    for (int c=0;c<3;++c) {
                        double code = rgb[i][c] / 255.0;
                        if (preset == ShaderPreset::Invert) code = 1.0 - code;
                        linear[c] = std::pow(code, 2.2);
                    }
                    // Independent double-precision BT.709 -> BT.2020 gamut
                    // reference and ST2084 encoding, after the selected effect.
                    const std::array<double,3> expectedNits{{
                        white*(0.627404*linear[0]+0.329282*linear[1]+0.0433136*linear[2]),
                        white*(0.069097*linear[0]+0.919540*linear[1]+0.0113612*linear[2]),
                        white*(0.0163916*linear[0]+0.0880132*linear[1]+0.895595*linear[2])}};
                    const auto* actual = static_cast<const std::uint16_t*>(mapped.pData) + i*8;
                    for (int c=0;c<3;++c) {
                        const double code = half(actual[c]);
                        valid = valid && std::isfinite(code) && code >= 0 && code <= 1;
                        maxCodeError = std::max(maxCodeError, std::abs(code-pq(expectedNits[c])));
                    }
                }
                r.context_->Unmap(staging.Get(), 0);
                require(valid && maxCodeError < 0.0012, "SDR fallback white/gamut/PQ or effect order is wrong");
                std::cout << "Software BGRA -> PQ effect=" << static_cast<int>(preset)
                          << " white-nits=" << white << " max-code-error=" << maxCodeError << " PASS\n";
            }
        }
        // swscale retains native HDR transfer codes. These routes must never
        // receive the new SDR transform a second time, even on a CPU frame.
        frame.ownerFrame = av_frame_alloc();
        require(frame.ownerFrame != nullptr, "HDR metadata allocation");
        for (auto transfer : {AVCOL_TRC_SMPTE2084, AVCOL_TRC_ARIB_STD_B67}) {
            ++frame.serial;
            frame.ownerFrame->color_trc = transfer;
            require(r.uploadSoftwareFrame(0, frame), "HDR-tagged software upload");
            require(!r.prepareSoftwarePqTexture(0), "Native HDR was transformed as SDR");
            r.bindVibranceColorSpace(0);
            require(r.slots_[0].hdrSource, "Software HDR metadata lost");
        }
        r.hdrOutput_ = previousHdr;
        r.enhancementStatus_.sdrWhiteNits = previousWhite;
        r.slots_[0] = {};
        require(r.setShaderPreset(ShaderPreset::SmartVibrancePlus), "Fallback shader restore");
    }

    static void verify(D3DRenderer& r) {
        require(r.setShaderPreset(ShaderPreset::SmartVibrancePlus), "HDR shader compilation");
        std::vector<Pixel> gray;
        for (double level : {0.0, 0.001, 0.01, 0.1, 1.0, 100.0, 203.0, 650.0, 1000.0, 10000.0})
            gray.push_back(encode(level, level, level));
        close(gray, draw(r, gray, Route::RtxPq), 1e-6, "HDR gray axis changed");

        std::vector<Pixel> patches = gray;
        for (double red : {0.0, 0.1, 10.0, 100.0, 500.0, 1000.0, 10000.0})
            for (double green : {0.0, 0.1, 10.0, 100.0, 500.0, 1000.0, 10000.0})
                for (double blue : {0.0, 0.1, 10.0, 100.0, 500.0, 1000.0, 10000.0})
                    patches.push_back(encode(red, green, blue));
        patches.push_back(encode(10000, 9999, 9998));
        SmartVibranceSettings neutral;
        neutral.intensity = 1;
        close(patches, draw(r, patches, Route::RtxPq, neutral), 1e-6, "HDR neutral setting changed pixels");
        // Alternate routes in one device/context: no stale per-pane constants.
        close(patches, draw(r, patches, Route::NativePq), 1e-6, "Native PQ must bypass Vibrance");
        draw(r, patches, Route::RtxPq);
        close(patches, draw(r, patches, Route::RefusedPq), 1e-6, "Refused TrueHDR must bypass Vibrance");
        close(patches, draw(r, patches, Route::NativePq), 1e-6, "RTX mode leaked into native pane");

        double worstRelativeY = 0;
        for (const auto settings : {SmartVibranceSettings{},
                SmartVibranceSettings{3, 0.2F, 0.0005F, 80},
                SmartVibranceSettings{3, 1, 0.01F, 5},
                SmartVibranceSettings{0, 1, 0.0005F, 80}}) {
            const auto output = draw(r, patches, Route::RtxPq, settings);
            for (std::size_t i = 0; i < patches.size(); ++i) {
                for (int c = 0; c < 4; ++c)
                    require(std::isfinite(output[i][c]) && output[i][c] >= 0 && output[i][c] <= 1.000001F,
                            "HDR output outside finite PQ range");
                require(output[i][3] == patches[i][3], "HDR changed alpha");
                const double y = luminance(patches[i]);
                const double error = std::abs(luminance(output[i]) - y) / std::max(0.01, y);
                worstRelativeY = std::max(worstRelativeY, error);
                require(error < 0.001, "HDR enhancement changed linear luminance by over 0.1%");
            }
        }

        const std::vector<Pixel> colors{encode(120,100,90), encode(90,110,130),
            encode(85,125,90), encode(0.012,0.01,0.009), encode(1080,900,810), encode(200,100,70)};
        const auto enhanced = draw(r, colors, Route::RtxPq);
        for (int i : {0,1,2,5}) require(chroma(enhanced[i]) > chroma(colors[i]) * 1.01,
                                      "HDR Vibrance must visibly increase midtone colourfulness");
        SmartVibranceSettings reduced;
        reduced.intensity = 0.5F;
        const auto lessColor = draw(r, colors, Route::RtxPq, reduced);
        for (int i : {0,1,2,5}) require(chroma(lessColor[i]) < chroma(colors[i]) * 0.99,
                                      "Intensity below one must reduce HDR colourfulness");
        // Same RGB spread and peak, with the warm/cool order reversed.
        const std::vector<Pixel> hues{encode(180,130,80), encode(80,130,180)};
        const auto hueOutput = draw(r, hues, Route::RtxPq);
        require(chroma(hueOutput[0])/chroma(hues[0]) < chroma(hueOutput[1])/chroma(hues[1]) - 0.01,
                "Warm colours must receive less enhancement than the cool comparison");
        const std::vector<Pixel> colorful{encode(200,80,120)};
        SmartVibranceSettings lowerPivot, higherPivot;
        lowerPivot.saturationPivot = 0.2F;
        higherPivot.saturationPivot = 1.0F;
        require(chroma(draw(r,colorful,Route::RtxPq,lowerPivot)[0]) <
                chroma(draw(r,colorful,Route::RtxPq,higherPivot)[0]), "Saturation pivot has no HDR effect");
        const std::vector<Pixel> nearGray{encode(100,97,100)};
        lowerPivot = higherPivot = SmartVibranceSettings{};
        lowerPivot.grayPivot = 0.0005F;
        higherPivot.grayPivot = 0.01F;
        require(chroma(draw(r,nearGray,Route::RtxPq,lowerPivot)[0]) >
                chroma(draw(r,nearGray,Route::RtxPq,higherPivot)[0]), "Gray pivot has no HDR effect");
        const std::vector<Pixel> softColor{encode(100,90,100)};
        lowerPivot = higherPivot = SmartVibranceSettings{};
        lowerPivot.graySharpness = 5;
        higherPivot.graySharpness = 80;
        require(chroma(draw(r,softColor,Route::RtxPq,lowerPivot)[0]) <
                chroma(draw(r,softColor,Route::RtxPq,higherPivot)[0]), "Gray sharpness has no HDR effect");
        const double midGain = chroma(enhanced[0]) / chroma(colors[0]);
        require(chroma(enhanced[3]) / chroma(colors[3]) < midGain - 0.01, "Near-black protection missing");
        require(chroma(enhanced[4]) / chroma(colors[4]) < midGain - 0.01, "Highlight protection missing");
        const std::vector<Pixel> primaries{encode(400,0,0), encode(0,400,0), encode(0,0,400),
            encode(400,400,0), encode(400,0,400), encode(0,400,400)};
        close(primaries, draw(r, primaries, Route::RtxPq), 1e-6, "Saturated boundary colors must be protected");

        // Regression reference from the original SDR formula. This reference
        // protects the established SDR look, not the new HDR implementation.
        const std::vector<Pixel> sdr{{0.4F,0.5F,0.6F,0.75F}, {0.1F,0.2F,0.7F,0.75F}, {0.5F,0.5F,0.5F,0.75F}};
        auto expected = sdr;
        for (auto& p : expected) {
            const double y = p[0]*0.2126 + p[1]*0.7152 + p[2]*0.0722;
            double e = 0;
            for (int c=0;c<3;++c) e += std::pow(p[c]-y,2);
            const double rolloff = 1 - std::clamp(std::sqrt(e)/0.5,0.0,1.0);
            const double protection = 1 / (1 + std::exp(-(0.003-e)*45));
            const double gain = 0.5 * (rolloff + (1-rolloff)*protection);
            for (int c=0;c<3;++c) p[c] = static_cast<float>(std::clamp(y+(p[c]-y)*(1+gain),0.0,1.0));
        }
        close(expected, draw(r,sdr,Route::Sdr), 1e-6, "Legacy SDR Vibrance changed");
        close(expected, draw(r,sdr,Route::Software), 1e-6, "Software BGRA was interpreted as PQ");
        require(r.setShaderPreset(ShaderPreset::Normal), "Normal shader restore failed");
        close(sdr, draw(r,sdr,Route::RtxPq), 1e-6, "Normal shader unexpectedly enhanced PQ");
        Microsoft::WRL::ComPtr<ID3D11Buffer> bound;
        r.context_->PSGetConstantBuffers(2,1,&bound);
        require(!bound, "Vibrance b2 leaked into another shader");
        require(r.setShaderPreset(ShaderPreset::SmartVibrancePlus), "Vibrance shader restore failed");
        std::cout << "HDR Vibrance pixels/routes PASS: " << patches.size()
                  << " patches, max linear-Y relative error=" << worstRelativeY << '\n';
    }
};
} // namespace quaddeck

namespace {

LRESULT CALLBACK probeWindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const bool allowMissingVideoSupport = argc == 2 &&
        std::wstring(argv[1]) == L"--allow-missing-video-support";
    if (argc != 1 && !allowMissingVideoSupport) {
        std::cerr << "Usage: QuadDeckShaderProbe [--allow-missing-video-support]\n";
        return 1;
    }
    const int capability = test::checkVideoSupport(allowMissingVideoSupport);
    if (capability != 0) return capability;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    constexpr wchar_t className[] = L"QuadDeckShaderProbeWindow";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = probeWindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::cerr << "RegisterClassW failed: " << GetLastError() << '\n';
        return 1;
    }

    HWND window = CreateWindowExW(
        0, className, L"QuadDeck shader probe", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 180,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::cerr << "CreateWindowExW failed: " << GetLastError() << '\n';
        return 1;
    }

    int result = 0;
    {
        D3DRenderer renderer;
        if (!renderer.initialize(window)) {
            std::cerr << "Renderer initialization failed: " << renderer.error() << '\n';
            result = 1;
        } else {
            renderer.setSmartVibranceSettings({2.25F, 0.65F, 0.004F, 60.0F});
            if (!renderer.setShaderPreset(ShaderPreset::SmartVibrancePlus)) {
                std::cerr << "Smart Vibrance Plus compilation failed: "
                          << renderer.error() << '\n';
                result = 1;
            } else {
                try {
                    D3DRendererShaderProbe::verify(renderer);
                    D3DRendererShaderProbe::softwarePqFallback(renderer);
                } catch (const std::exception& error) {
                    std::cerr << "HDR Vibrance check failed: " << error.what() << '\n';
                    result = 1;
                }
                auto frame = std::make_shared<HardwareVideoFrame>();
                frame->width = 2;
                frame->height = 2;
                frame->stride = 8;
                frame->serial = 1;
                frame->bgra = {
                    0x10, 0x20, 0xE0, 0xFF, 0xE0, 0x20, 0x10, 0xFF,
                    0x20, 0xE0, 0x10, 0xFF, 0x80, 0x80, 0x80, 0xFF};

                PaneArray<std::shared_ptr<const HardwareVideoFrame>> frames{};
                frames[0] = std::move(frame);
                PaneArray<PaneView> views{};
                const PaneArray<bool> active{true, false, false, false, false};
                const PaneArray<float> aspectRatios{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
                renderer.render(frames, views, active, LayoutMode::Grid2x2,
                                -1, -1, 180, aspectRatios);
                if (!renderer.error().empty()) {
                    std::cerr << "Smart Vibrance Plus render failed: "
                              << renderer.error() << '\n';
                    result = 1;
                }

                // Device-loss recovery tears down every D3D resource and
                // builds a new device, swap chain and pipeline. A driver reset
                // cannot be provoked here, but the rebuild itself is the part
                // that rots unnoticed, so run it and render again on the new
                // device. The frame is deliberately reused: its slot textures
                // belonged to the device that was just destroyed.
                if (!renderer.recover(window)) {
                    std::cerr << "Device recovery failed: " << renderer.error() << '\n';
                    result = 1;
                } else if (renderer.deviceLost()) {
                    std::cerr << "Device still flagged lost after recovery\n";
                    result = 1;
                } else {
                    renderer.setSmartVibranceSettings({2.25F, 0.65F, 0.004F, 60.0F});
                    if (!renderer.setShaderPreset(ShaderPreset::SmartVibrancePlus)) {
                        std::cerr << "Shader recompilation after recovery failed: "
                                  << renderer.error() << '\n';
                        result = 1;
                    }
                    try {
                        D3DRendererShaderProbe::verify(renderer);
                        D3DRendererShaderProbe::softwarePqFallback(renderer);
                    } catch (const std::exception& error) {
                        std::cerr << "HDR Vibrance after recovery failed: " << error.what() << '\n';
                        result = 1;
                    }
                    renderer.render(frames, views, active, LayoutMode::Grid2x2,
                                    -1, -1, 180, aspectRatios);
                    if (!renderer.error().empty()) {
                        std::cerr << "Render after recovery failed: "
                                  << renderer.error() << '\n';
                        result = 1;
                    }
                }

                // A failed rebuild must remain visibly lost so App can make
                // its next bounded attempt. A null HWND deterministically
                // prevents swap-chain creation after the device is rebuilt.
                if (renderer.recover(nullptr)) {
                    std::cerr << "Recovery unexpectedly accepted a null window\n";
                    result = 1;
                } else if (!renderer.deviceLost()) {
                    std::cerr << "Failed recovery cleared the device-lost latch\n";
                    result = 1;
                }
            }
        }
    }

    DestroyWindow(window);
    UnregisterClassW(className, instance);
    return result;
}
