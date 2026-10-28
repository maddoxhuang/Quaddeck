#pragma once

#include "Core.hpp"
#include "VideoOutput.hpp"
#include "VideoSource.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <d3d11_4.h>
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

// Whether a device D3D11CreateDevice did not make is asked for again without
// D3D11_CREATE_DEVICE_VIDEO_SUPPORT. That is so when the flag is what it
// refused: DXGI_ERROR_UNSUPPORTED, from WARP and from the Microsoft Basic
// Render Driver, which D3D11 takes for the card where no vendor driver is (a
// VM, a fresh install, a driver being installed). WARP is asked again after
// any failure, as before. Any other failure on a card (E_FAIL, out of memory,
// a hung device) is returned, so a device made on it later keeps its video
// processor.
inline bool retryWithoutVideoSupport(HRESULT hr, D3D_DRIVER_TYPE driver) {
    return hr == DXGI_ERROR_UNSUPPORTED || driver == D3D_DRIVER_TYPE_WARP;
}

// Whether a device made on a software adapter (the Basic Render Driver, or
// WARP) stands. A rebuild after a device on a card does not settle on one:
// during a driver update it fails, and App tries again until the card is
// back, where a device made on the software adapter would stay on the CPU.
// The first build takes it, where the window would not open otherwise, and
// so do a rebuild after a software device and the WARP choice.
inline bool deviceAdapterAcceptable(bool softwareAdapter, bool previousOnCard, VideoRenderer chosen) {
    return !softwareAdapter || !previousOnCard || chosen == VideoRenderer::Warp;
}

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
    // The Video tab's settings (VideoOutput.hpp); they survive a device
    // rebuild. Before initialize() they only decide what it builds. After,
    // true says the device must be built again for them, on another
    // renderer or card: the caller then does what a lost device needs,
    // requestRebuild() and its own recovery, the videos opened again on the
    // new device. Everything else takes effect on the next frame.
    bool setVideoOutput(const VideoOutputSettings& settings);
    // Raises the lost-device latch without a failure, so the caller's
    // recovery builds the device again as the settings now say.
    void requestRebuild();
    // What was built, for the line under the Video tab's choices.
    VideoRendererStatus rendererStatus() const;
    // A display was added, removed or changed (WM_DISPLAYCHANGE): the next
    // frame looks at it rather than within two seconds.
    void noteDisplayChange();
    bool loadPixelShader(const std::wstring& path);
    std::string error() const { return error_; }
    // Signalled by the swap chain when it can accept another frame. The main
    // loop waits on this instead of on a timer, and does so outside the device
    // lock. The bitblt chain has none; a timer set for the display's next
    // refresh stands in for it. Null when there is neither.
    HANDLE frameLatencyWaitableObject() const {
        return frameLatencyWaitable_ ? frameLatencyWaitable_ : paceTimer_;
    }
    ID3D11Device* device() const { return device_.Get(); }
    ID3D11DeviceContext* deviceContext() const { return context_.Get(); }
    std::recursive_mutex& deviceMutex() const { return deviceMutex_; }

private:
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
        // The output is PQ: an HDR10 stream passed through, or RTX Video
        // HDR's. A 10-bit SDR output is R10G10B10A2 as well, so the format
        // alone does not say it.
        bool pqOutput{};
        bool tenBitOutputMode{};
        // Built to deinterlace. Once a stream has shown an interlaced frame
        // its processor stays so: the progressive frames among them are
        // told so one by one, and nothing is rebuilt between the two.
        bool interlaced{};
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
        // The stream has shown an interlaced frame: every frame of it goes
        // through the video processor from then on, which deinterlaces.
        bool interlaced{};
    };
    // The user's shader at the converted picture's size, for the output
    // pass to enlarge with its own scaler afterwards.
    struct PostTexture {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        int width{};
        int height{};
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
    // The device on the chosen card or WARP, else on the one D3D11 picks,
    // else on WARP; a rebuild after a device on a card refuses a software
    // adapter (deviceAdapterAcceptable).
    bool createD3DDevice();
    // One device on the adapter or driver type into device_ and context_:
    // without the debug layer when it is missing, and without video support
    // when the device refuses that (retryWithoutVideoSupport).
    HRESULT createDeviceOn(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driver);
    bool createSwapChain(HWND window, IDXGIFactory2* factory);
    bool createPipeline();
    bool compilePixelShader(const char* source, std::size_t length, const char* name);
    bool compilePixelShaderFile(const std::wstring& path);
    bool createBackBuffer();
    // Gives the swap chain buffers of this size and a render target on them.
    // When ResizeBuffers fails on a device that is not lost, the chain is
    // given the old size back and the target made on that, so the picture
    // goes on at the old size stretched to the window, and the new size
    // waits in pendingWidth_/pendingHeight_ for retryBackBuffer().
    bool resizeBackBuffer(unsigned width, unsigned height);
    // Called by render() under the device lock: about once a second while
    // the render target is missing or a size is pending, tries again.
    void retryBackBuffer();
    // Which output the window sits on, whether Windows has it in HDR mode
    // and its SDR white level. Cheap; repeated every few seconds because the
    // user can flip HDR or move the window at any time.
    void refreshDisplayStatus();
    // A DXGI factory keeps the outputs as they were when it was made, so one
    // made before Windows HDR was switched reports the old colour space for
    // good. Once it is no longer current it is made again and the device's
    // adapter found in it by LUID; `force` makes it again regardless.
    void refreshDisplayFactory(bool force = false);
    // Asks the driver once per device whether it honours the two extensions.
    void probeVideoEnhancements();
    // Moves the swap chain between 8-bit sRGB, 10-bit sRGB and 10-bit PQ to
    // match what the settings and the display allow. Rebuilds the back
    // buffer and drops every slot, whose output format changed with it.
    bool applyOutputMode();
    bool wantHdrOutput() const;
    bool wantTenBitOutput() const;
    // Lets every pane's converted picture go, to be made again on the next
    // frame as the settings now say.
    void dropSlots();
    // Sets the bitblt chain's pacing timer for the display's next refresh.
    void armPaceTimer();
    // The user's shader at the converted picture's size, into the pane's
    // post texture.
    bool runEffectsPass(std::size_t slot);
    // The pane's picture into the back buffer through the chosen scaler,
    // then the colour controls and the levels; the viewport and the UV
    // transform are already set.
    void drawOutputPass(ID3D11ShaderResourceView* view, int width, int height,
                        VideoScaling scaling, bool color);
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
    // The device's adapter as the display factory lists it: the outputs
    // the window may sit on are read from it.
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter_;
    Microsoft::WRL::ComPtr<IDXGIFactory1> displayFactory_;
    LUID adapterLuid_{};
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    // Signals when the GPU has finished a frame's work, so the wait for it
    // can happen outside the device lock: a fence that sets an event, or on
    // a driver without D3D11.4 fences an event query polled on a
    // high-resolution timer. A plain sleep there rounds up to the system
    // tick, 15.6 ms, and the frame misses its blank.
    Microsoft::WRL::ComPtr<ID3D11Fence> frameFence_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> fenceContext_;
    UINT64 frameFenceValue_{};
    HANDLE frameFenceEvent_{};
    bool frameFenceUnavailable_{};
    Microsoft::WRL::ComPtr<ID3D11Query> frameQuery_;
    HANDLE frameQueryTimer_{};
    bool hdrOutput_{};
    // A 10-bit sRGB back buffer, the Video tab's choice in SDR.
    bool tenBitOutput_{};
    VideoOutputSettings videoOutput_;
    // What the device was asked to be when it was built, and what it is.
    VideoRendererChoice builtChoice_;
    VideoRenderer builtRenderer_{VideoRenderer::Direct3D11};
    bool adapterFallback_{};
    bool adapterMissing_{};
    // The last device made was on a hardware adapter, so a rebuild waits
    // for a card rather than settle on a software one. Kept through
    // releaseDeviceResources(); false before the first build.
    bool deviceOnCard_{};
    // A flip-model chain; false for bitblt, which presents without vsync
    // into DWM's copy of the window and is paced by paceTimer_.
    bool flipModel_{true};
    HANDLE paceTimer_{};
    // The scalers and the colour controls: a shader, its parameters, and
    // the point sampler the kernels take their taps through.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> outputShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> outputParamsBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> pointSampler_;
    PaneArray<PostTexture> postTextures_{};
    // The shader in pixelShader_ changes the picture (not the plain copy),
    // so the output pass needs its result first.
    bool effectShaderActive_{};
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
    // A size resize() could not give the swap chain, 0 x 0 when none; when
    // render() next tries it, or a missing render target, again; and how
    // many tries have failed in a row. Retries log only now and then
    // (quietErrors_ keeps setError from writing the log meanwhile).
    unsigned pendingWidth_{};
    unsigned pendingHeight_{};
    std::chrono::steady_clock::time_point nextBackBufferRetry_{};
    unsigned backBufferRetries_{};
    bool quietErrors_{};
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
