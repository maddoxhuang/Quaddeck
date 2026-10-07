#pragma once

// Constants and helpers shared by the translation units that implement App
// (App.cpp, AppCommands.cpp, AppPersistence.cpp, AppPlayback.cpp). They were
// file-local to App.cpp before it was split; nothing else should include this.

#include "App.hpp"

#include <commctrl.h>

#include <algorithm>
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
// An adapter that will not come back should not be hammered once a second for
// the rest of the session.
inline constexpr int kMaximumDeviceRecoveries = 5;
// Long enough that one slow interval does not dominate a report.
inline constexpr double kPerformanceReportSeconds = 5.0;
enum ContextCommand : unsigned {
    CmdOpenPane = 2000, CmdClosePane, CmdSelectAudio, CmdTogglePanePause, CmdToggleSolo,
    CmdPreviousFile, CmdNextFile,
    CmdPaneMute, CmdPaneVolumeUp, CmdPaneVolumeDown, CmdPaneVolumeReset,
    CmdResetPaneOffset, CmdResetAllOffsets,
    CmdSpeed025, CmdSpeed050, CmdSpeed100, CmdSpeed150, CmdSpeed200,
    CmdViewFit, CmdViewFill, CmdViewStretch, CmdZoomIn, CmdZoomOut, CmdZoomReset,
    CmdTogglePaneRepeat, CmdPaneLoopSetA, CmdPaneLoopSetB, CmdPaneLoopToggle, CmdPaneLoopClear,
    CmdMasterLoopSetA, CmdMasterLoopSetB, CmdMasterLoopToggle, CmdMasterLoopClear,
    CmdToggleExpanded, CmdAllViewFit, CmdAllViewFill, CmdAllViewStretch,
    CmdAllZoomIn, CmdAllZoomOut, CmdAllZoomReset, CmdToggleRepeatAll,
    CmdPlayPause, CmdStop, CmdMute,
    CmdLayoutGrid, CmdLayoutSide, CmdLayoutRow, CmdLayoutColumn,
    CmdLayoutPortrait, CmdLayoutLandscape, CmdSeekLinked, CmdSeekIndependent,
    CmdDecodeAuto, CmdDecodeHardware, CmdDecodeSoftware,
    CmdShaderNormal, CmdShaderSharpen, CmdShaderGrayscale, CmdShaderInvert,
    CmdShaderSmartVibrance, CmdShaderLoad,
    CmdSessionOpen, CmdSessionSave, CmdStyleOpen, CmdStyleSave,
    CmdToggleControls, CmdFullscreen, CmdSettings,
    CmdOpenEmby, CmdSubtitleOff,
    CmdSubtitleShow, CmdSubtitleCycle, CmdSubtitleLoad,
    CmdSubtitleEarlier, CmdSubtitleLater, CmdSubtitleSyncReset,
    CmdPlayOrderFirst, CmdPlayOrderLast = CmdPlayOrderFirst + kPlayOrderCount - 1,
    CmdSubtitleFirst, CmdSubtitleLast = CmdSubtitleFirst + 63,
    CmdAudioTrackFirst, CmdAudioTrackLast = CmdAudioTrackFirst + 31
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

// The text of one dialog control, for the Emby sign-in and search dialogs.
inline std::wstring dialogText(HWND dialog, int control) {
    wchar_t buffer[1024]{};
    GetDlgItemTextW(dialog, control, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
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
    std::wostringstream out;
    if (hours > 0) {
        out << hours << L":" << std::setfill(L'0') << std::setw(2) << minutes;
    } else {
        out << minutes;
    }
    out << L":" << std::setfill(L'0') << std::setw(2) << secs;
    return out.str();
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
