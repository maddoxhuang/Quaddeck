#pragma once

#include "Core.hpp"
#include "VideoSource.hpp"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace quaddeck {

// Where a frame's time goes inside the renderer. Accumulated across frames and
// drained by the caller, so a report covers a whole interval rather than one
// arbitrary frame.
//
// lockWaitMs is the one that distinguishes a slow renderer from a contended
// one: the decoders and the renderer share a single immediate context behind a
// single mutex, so time spent acquiring it is time the renderer spent queued
// behind five decoders, not time it spent working.
struct RenderStats {
    std::uint64_t frames{};
    double lockWaitMs{};
    double convertMs{};
    double drawMs{};
    double presentMs{};
    double lockHeldMs{};
    // Time spent waiting for the GPU to finish the frame, outside the lock.
    double gpuWaitMs{};
    std::uint64_t softwareFrameUploads{};
    std::uint64_t softwareNv12Uploads{};
};

class D3DRenderer {
public:
    D3DRenderer() = default;
    ~D3DRenderer();
    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;

    bool initialize(HWND window);
    void resize(unsigned width, unsigned height);
    // Invoked once per frame with the back buffer, after every pane has been
    // drawn and before Present, inside the device lock. The drawn interface
    // lives there; the renderer knows nothing about it. In the 10-bit PQ
    // output mode the surface is instead an empty BGRA layer (transparentLayer
    // true) that the renderer then converts to PQ and blends over the video,
    // so the interface keeps its SDR brightness on an HDR display.
    using OverlayPainter = std::function<void(IDXGISurface* surface, bool transparentLayer)>;
    void setOverlayPainter(OverlayPainter painter) { overlay_ = std::move(painter); }
    void render(const PaneArray<std::shared_ptr<const HardwareVideoFrame>>& frames,
                const PaneArray<PaneView>& views,
                const PaneArray<bool>& active, LayoutMode layout,
                int expandedPane, int soloPane, unsigned contentHeight,
                const PaneArray<float>& aspectRatios,
                AutoLayoutFocus autoFocus = AutoLayoutFocus::Dynamic,
                int autoFocusPane = -1);
    // A driver reset, a GPU hang or a TDR invalidates the device and every
    // resource on it. Nothing can be rendered again until it is rebuilt.
    bool deviceLost() const { return deviceLost_; }
    // Returns the accumulated timings and starts a new interval.
    RenderStats takeStats();
    // Tears the device down and builds a new one. The caller must release
    // everything else holding the old device first -- in particular every
    // decoder, whose D3D11VA context and decoded surfaces belong to it.
    bool recover(HWND window);

    bool setShaderPreset(ShaderPreset preset);
    void setSmartVibranceSettings(const SmartVibranceSettings& settings);
    // NVIDIA RTX Video Super Resolution and RTX Video HDR. Survives a device
    // rebuild like the vibrance settings. Takes effect on the next frame: the
    // video processors are rebuilt with the extensions, and RTX Video HDR
    // switches the swap chain to 10-bit PQ when the display is in HDR mode.
    void setVideoEnhancements(const VideoEnhancementSettings& settings);
    VideoEnhancementStatus enhancementStatus() const;
    bool loadPixelShader(const std::wstring& path);
    std::string error() const { return error_; }
    // Signalled by the swap chain when it can accept another frame. The main
    // loop waits on this instead of on a timer, and does so outside the device
    // lock. Null when the swap chain is not waitable.
    HANDLE frameLatencyWaitableObject() const { return frameLatencyWaitable_; }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* deviceContext() const { return context_.Get(); }
    std::recursive_mutex& deviceMutex() const { return deviceMutex_; }

private:
    // The native colour probe reads the converted texture before presentation.
    friend struct D3DRendererColorProbe;
    friend struct D3DRendererShaderProbe;
    struct SlotTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView;
        int inputWidth{};
        int inputHeight{};
        DXGI_FORMAT inputFormat{DXGI_FORMAT_UNKNOWN};
        int outputWidth{};
        int outputHeight{};
        DXGI_FORMAT outputFormat{DXGI_FORMAT_UNKNOWN};
        // An HDR10 stream is passed through in the PQ output mode rather than
        // given to RTX Video HDR, so it is part of the processor's identity.
        bool hdrSource{};
        bool hdrOutputMode{};
        // Per processor, never the most recently created pane's global status.
        bool rtxHdrApplied{};
        std::uint64_t serial{};
        bool hardware{};
    };
    struct SoftwareUpload {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        std::vector<std::uint8_t> bytes;
        int width{};
        int height{};
        std::uint64_t serial{};
        bool failed{};
        int failedOutputWidth{};
        int failedOutputHeight{};
    };
    struct SoftwarePqTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> effects;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> effectsView;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> effectsTarget;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> pq;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> pqView;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pqTarget;
        int width{};
        int height{};
    };

    bool createDevice(HWND window);
    bool createPipeline();
    bool compilePixelShader(const char* source, std::size_t length, const char* name);
    bool compilePixelShaderFile(const std::wstring& path);
    bool createBackBuffer();
    // Which output the window sits on, whether Windows has it in HDR mode
    // and its SDR white level. Cheap; repeated every few seconds because the
    // user can flip HDR or move the window at any time.
    void refreshDisplayStatus();
    // Asks the driver once per device whether it honours the two extensions.
    void probeVideoEnhancements();
    // Moves the swap chain between 8-bit sRGB and 10-bit PQ to match what the
    // settings and the display allow. Rebuilds the back buffer and drops every
    // slot, whose output format changed with it.
    bool applyOutputMode();
    bool wantHdrOutput() const;
    void applyStreamExtensions(std::size_t slot, bool hdrSource);
    void bindVibranceColorSpace(std::size_t slot);
    bool compositeOverlayLayer();
    bool convertSurface(std::size_t slot, const HardwareVideoFrame& frame,
                        const SurfaceSize& requested);
    bool uploadSoftwareFrame(std::size_t slot, const HardwareVideoFrame& frame);
    bool convertSoftwareSurface(std::size_t slot, const HardwareVideoFrame& frame,
                                const SurfaceSize& requested);
    bool prepareSoftwarePqTexture(std::size_t slot);
    bool createSlotResources(std::size_t slot, const HardwareVideoFrame& frame,
                             const SurfaceSize& requested);
    void releaseDeviceResources();
    void setError(const char* operation, HRESULT hr);
    // Records the failure and, when it is a lost device, flags it for rebuild.
    void noteFailure(const char* operation, HRESULT hr);

    HWND window_{};
    unsigned width_{};
    unsigned height_{};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    // Null on a driver without the DXGI colour-space calls; the PQ output
    // mode then stays off.
    Microsoft::WRL::ComPtr<ID3D11VideoContext1> videoContext1_;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    // Signals when the GPU has finished a frame's work, so the wait for it
    // can happen outside the device lock.
    Microsoft::WRL::ComPtr<ID3D11Query> frameQuery_;
    bool hdrOutput_{};
    unsigned framesSinceDisplayStatus_{};
    // The interface layer and the pass that blends it in PQ output mode.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> overlayLayer_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> overlayLayerView_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> overlayCompositeShader_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> overlayBlend_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> overlayParamsBuffer_;
    VideoEnhancementSettings enhancements_;
    VideoEnhancementStatus enhancementStatus_;
    // Last logged outcome per extension, so a window drag that rebuilds five
    // processors does not write five identical lines.
    int loggedSuperResolution_{-1};
    int loggedRtxHdr_{-1};
    HANDLE frameLatencyWaitable_{};
    UINT swapChainFlags_{};
    bool deviceLost_{};
    bool deviceRebuildInProgress_{};
    RenderStats stats_{};
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTarget_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> uvTransformBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> pixelParamsBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> smartVibranceBuffer_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> vibranceColorBuffer_;
    bool smartVibranceSelected_{};
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> copyShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> sdrToPqShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> sdrToPqParams_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    PaneArray<SlotTexture> slots_{};
    PaneArray<SoftwareUpload> softwareUploads_{};
    PaneArray<SoftwarePqTexture> softwarePq_{};
    PaneArray<int> loggedSoftwareRoute_{{-1, -1, -1, -1, -1}};
    OverlayPainter overlay_;
    mutable std::recursive_mutex deviceMutex_;
    SmartVibranceSettings smartVibranceSettings_;
    std::string error_;
};

}  // namespace quaddeck
