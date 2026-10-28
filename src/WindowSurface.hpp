#pragma once

// A window of the player's own other than the video's -- the settings
// window -- drawn on the renderer's device: a flip swap chain sized to the
// window's client, whose back buffer the painter draws into. The caller
// holds the device lock around everything here but release, as it does
// for the video's chain; Present never waits for the display, so the lock
// is not held across a vertical blank.

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <windows.h>

namespace quaddeck {

class WindowSurface {
public:
    WindowSurface() = default;
    ~WindowSurface() { release(); }
    WindowSurface(const WindowSurface&) = delete;
    WindowSurface& operator=(const WindowSurface&) = delete;

    // The back buffer to draw this frame into, the chain made for `window`
    // on `device` or given this size first; null when that failed. A
    // different device -- one rebuilt after a loss -- gets a new chain.
    Microsoft::WRL::ComPtr<IDXGISurface> backBuffer(ID3D11Device* device, HWND window, UINT width, UINT height);
    // Shows what was drawn. False when the chain could not take the frame
    // now; the next one is drawn whole anyway.
    bool present();
    // Lets the chain go: before the device it was made on is torn down.
    void release();
    bool ready() const { return swapChain_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    ID3D11Device* device_{};
    HWND window_{};
    UINT width_{};
    UINT height_{};
};

}  // namespace quaddeck
