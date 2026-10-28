#include "WindowSurface.hpp"

#include <algorithm>

namespace quaddeck {

Microsoft::WRL::ComPtr<IDXGISurface> WindowSurface::backBuffer(ID3D11Device* device, HWND window, UINT width,
                                                               UINT height) {
    if (!device || !window) return nullptr;
    width = std::max<UINT>(1, width);
    height = std::max<UINT>(1, height);
    if (swapChain_ && (device != device_ || window != window_)) release();
    if (!swapChain_) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return nullptr;
        }
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = width;
        description.Height = height;
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        description.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (FAILED(factory->CreateSwapChainForHwnd(device, window, &description, nullptr, nullptr, &swapChain_))) {
            swapChain_.Reset();
            return nullptr;
        }
        // Alt+Enter is the player's fullscreen, not DXGI's.
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        device_ = device;
        window_ = window;
        width_ = width;
        height_ = height;
    } else if (width != width_ || height != height_) {
        // Nothing holds a buffer between frames, so the chain can be resized.
        if (FAILED(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            release();
            return nullptr;
        }
        width_ = width;
        height_ = height;
    }
    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&surface)))) return nullptr;
    return surface;
}

bool WindowSurface::present() {
    if (!swapChain_) return false;
    // Not tied to the vertical blank, and not waiting for a buffer: the
    // video's chain paces the frames, and the device lock is held here.
    const HRESULT hr = swapChain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
    if (FAILED(hr)) {
        // A lost device: the renderer finds it on its own present, and the
        // chain is made again on the device that replaces it.
        release();
        return false;
    }
    return true;
}

void WindowSurface::release() {
    swapChain_.Reset();
    device_ = nullptr;
    window_ = nullptr;
    width_ = height_ = 0;
}

}  // namespace quaddeck
