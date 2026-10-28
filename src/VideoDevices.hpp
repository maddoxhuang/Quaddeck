#pragma once

// The graphics cards and displays Windows has, for the Video tab's lists,
// the renderer's choice of card and fullscreen's choice of display.

#include "VideoOutput.hpp"

#include <string>
#include <vector>

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace quaddeck {

// Every hardware adapter DXGI lists, in its order (the one driving the main
// display first); the software one is the WARP renderer, not a card.
std::vector<VideoAdapterInfo> listVideoAdapters();
// The adapter chosen by name and by which of that name it is, or null when
// none is connected now.
Microsoft::WRL::ComPtr<IDXGIAdapter1> findVideoAdapter(const std::wstring& name, int ordinal);
// Every display on the desktop, in Windows' order, with the monitor's own
// name where Windows knows it.
std::vector<VideoDisplayInfo> listVideoDisplays();
// The monitor of that device name (\\.\DISPLAY2), null when it is not on
// the desktop now.
HMONITOR monitorForDisplay(const std::wstring& device);

}  // namespace quaddeck
