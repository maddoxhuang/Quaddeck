#pragma once

// Constants and helpers shared by the translation units that implement App
// (App.cpp, AppCommands.cpp, AppPersistence.cpp, AppPlayback.cpp). They were
// file-local to App.cpp before it was split; nothing else should include this.

#include "App.hpp"

#include <commctrl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <iomanip>
#include <sstream>
#include <string>

namespace quaddeck::app_internal {

inline constexpr wchar_t kWindowClass[] = L"QuadDeckWindow";
inline constexpr wchar_t kVideoWindowClass[] = L"QuadDeckVideoWindow";
inline constexpr UINT_PTR kRenderTimer = 1;
inline constexpr UINT_PTR kSettingsSaveTimer = 2;
inline constexpr UINT kSettingsSaveDelayMs = 300;
inline constexpr UINT kSettingsSaveRetryDelayMs = 1200;
inline constexpr unsigned kMaximumSettingsSaveRetries = 1;
// A moving pointer this close to the bottom edge reveals the transport bar,
// and this close to the top edge the window caption.
inline constexpr int kControlRevealZone = 22;
inline constexpr float kDockAnimationMs = 180.0F;
inline constexpr ULONGLONG kControlHideDelayMs = 1100;
inline constexpr ULONGLONG kPointerHideDelayMs = 1400;
// Upper bound on how long the main loop blocks without producing a frame.
inline constexpr DWORD kFrameWaitTimeoutMs = 32;
// How stale the last frame must be before the timer takes over for a modal loop.
inline constexpr ULONGLONG kModalRenderFallbackMs = 32;
// A failed device rebuild is tried again after 1, 2, 4 ... seconds, then
// every 30: an adapter that will not come back is not hammered, and one that
// does -- a driver finishing its install, a GPU plugged back in -- is found
// without restarting the player.
inline constexpr int kMaximumDeviceRecoveryDelaySeconds = 30;
inline std::chrono::seconds deviceRecoveryDelay(int failedAttempts) {
    int seconds = 1;
    for (int attempt = 1; attempt < failedAttempts && seconds < kMaximumDeviceRecoveryDelaySeconds; ++attempt) {
        seconds *= 2;
    }
    return std::chrono::seconds(std::min(seconds, kMaximumDeviceRecoveryDelaySeconds));
}
// Long enough that one slow interval does not dominate a report.
inline constexpr double kPerformanceReportSeconds = 5.0;
// The narrowest an Emby navigation button gets before its row wraps, in
// DIPs: a narrow docked sheet takes Back ... Refresh on two lines rather
// than clipping five captions.
inline constexpr float kEmbyNavButtonWidth = 64.0F;
// The widths a browser tile's picture is asked for at: twice the tile,
// rounded up to a hundred so a small change of window width does not
// refetch them all, and never under these.
inline constexpr int kEmbyPosterImageWidth = 300;
inline constexpr int kEmbyThumbImageWidth = 440;
inline int embyTileImageWidth(float tileWidth, bool posters, float scale) {
    return std::max(posters ? kEmbyPosterImageWidth : kEmbyThumbImageWidth,
                    static_cast<int>(std::ceil(tileWidth * scale * 2.0F / 100.0F)) * 100);
}
// How far inside its control a sheet slider's rail starts, in DIPs; the
// drag (AppChrome.cpp) and the hit test (AppPanel.cpp) must agree.
inline constexpr float kPanelRailInset = 8.0F;
enum ContextCommand : unsigned {
    CmdOpenPane = 2000, CmdClosePane, CmdSelectAudio, CmdTogglePanePause, CmdToggleSolo,
    CmdPreviousFile, CmdNextFile,
    CmdPaneMute, CmdResetPaneOffset,
    CmdSpeed025, CmdSpeed050, CmdSpeed100, CmdSpeed150, CmdSpeed200,
    CmdViewFit, CmdViewFill, CmdViewStretch, CmdZoomIn, CmdZoomOut, CmdZoomReset,
    CmdTogglePaneRepeat, CmdPaneLoopSetA, CmdPaneLoopSetB, CmdPaneLoopToggle, CmdPaneLoopClear,
    CmdMasterLoopSetA, CmdMasterLoopSetB, CmdMasterLoopToggle, CmdMasterLoopClear,
    CmdToggleExpanded,
    CmdPlayPause, CmdStop, CmdMute,
    CmdLayoutGrid, CmdLayoutSide, CmdLayoutRow, CmdLayoutColumn,
    CmdLayoutPortrait, CmdLayoutLandscape, CmdSeekLinked, CmdSeekIndependent,
    CmdSessionOpen, CmdSessionSave,
    CmdFullscreen, CmdSettings,
    CmdOpenEmby, CmdSubtitleOff,
    CmdSubtitleCycle, CmdSubtitleLoad,
    CmdSubtitleEarlier, CmdSubtitleLater, CmdSubtitleSyncReset,
    CmdPlayOrderFirst, CmdPlayOrderLast = CmdPlayOrderFirst + kPlayOrderCount - 1,
    CmdSubtitleFirst, CmdSubtitleLast = CmdSubtitleFirst + 63,
    CmdAudioTrackFirst, CmdAudioTrackLast = CmdAudioTrackFirst + 31,
    // An Emby item marked or unmarked as a favorite: a pane's, or the
    // browser item a right-click was on.
    CmdEmbyFavorite
};

// The arrangement's name as the sheet, the menu and the notice show it,
// in LayoutMode's order, which is also CmdLayoutGrid..CmdLayoutLandscape's.
inline const wchar_t* layoutModeName(LayoutMode mode) {
    switch (mode) {
    case LayoutMode::SideBySide: return L"One row";
    case LayoutMode::Row4: return L"Two rows";
    case LayoutMode::Column4: return L"One column";
    case LayoutMode::PortraitStack: return L"Portrait focus";
    case LayoutMode::LandscapePair: return L"Landscape focus";
    default: return L"Automatic";
    }
}

inline const wchar_t* viewModeName(ViewMode mode) {
    switch (mode) {
    case ViewMode::Fill: return L"Fill";
    case ViewMode::Stretch: return L"Stretch";
    default: return L"Fit";
    }
}

// FFmpeg and DXGI messages are UTF-8. Widening them byte by byte, as the title
// used to, turns any non-ASCII path or localised message into mojibake.
inline std::wstring utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), wchar_t{});
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size);
    return result;
}

// What the audio-track submenu shows for one stream.
inline std::wstring audioTrackLabel(const AudioTrackInfo& track) {
    std::wstring label;
    if (!track.title.empty()) label = utf8ToWide(track.title);
    if (!track.language.empty()) {
        if (!label.empty()) label += L"  ";
        label += utf8ToWide(track.language);
    }
    if (label.empty()) label = L"Track " + std::to_wstring(track.streamIndex);
    std::wstring detail = utf8ToWide(track.codec);
    if (track.channels > 0) {
        if (!detail.empty()) detail += L' ';
        detail += std::to_wstring(track.channels) + L"ch";
    }
    if (!detail.empty()) label += L"  (" + detail + L")";
    return label;
}

inline std::wstring formatTime(double seconds) {
    seconds = std::max(0.0, seconds);
    const auto total = static_cast<long long>(seconds);
    const auto hours = total / 3600;
    const auto minutes = (total % 3600) / 60;
    const auto secs = total % 60;
    // Into a buffer on the stack rather than a stream: the list's lengths,
    // the panes' time labels and the title ask this many times a frame.
    wchar_t text[48];
    const int length = hours > 0 ? std::swprintf(text, std::size(text), L"%lld:%02lld:%02lld", hours, minutes, secs)
                                 : std::swprintf(text, std::size(text), L"%lld:%02lld", minutes, secs);
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

inline SeekBarrier::AudioState barrierAudioState(AudioDecodeState state) {
    switch (state) {
    case AudioDecodeState::Primed: return SeekBarrier::AudioState::Primed;
    case AudioDecodeState::Ended: return SeekBarrier::AudioState::Ended;
    case AudioDecodeState::Error: return SeekBarrier::AudioState::Error;
    default: return SeekBarrier::AudioState::Unknown;
    }
}

inline const char* audioDecodeStateName(AudioDecodeState state) {
    switch (state) {
    case AudioDecodeState::Primed: return "primed";
    case AudioDecodeState::Ended: return "ended";
    case AudioDecodeState::Error: return "error";
    default: return "unknown";
    }
}



}  // namespace quaddeck::app_internal
