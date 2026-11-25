#pragma once

// Constants and helpers shared by the translation units that implement App
// (App.cpp, AppCommands.cpp, AppPersistence.cpp, AppPlayback.cpp). They were
// file-local to App.cpp before it was split; nothing else should include this.

#include "App.hpp"
#include "TextEncoding.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace quaddeck::app_internal {

inline constexpr wchar_t kWindowClass[] = L"QuadDeckWindow";
inline constexpr wchar_t kVideoWindowClass[] = L"QuadDeckVideoWindow";
inline constexpr UINT_PTR kRenderTimer = 1;
inline constexpr UINT_PTR kSettingsSaveTimer = 2;
inline constexpr UINT kSettingsSaveDelayMs = 300;
inline constexpr UINT kSettingsSaveRetryDelayMs = 1200;
inline constexpr unsigned kMaximumSettingsSaveRetries = 1;
// The clipboard's text, no more than a text box can hold, and text put on
// it, for the text boxes (AppTextFields.cpp) and a spin box's number.
std::wstring clipboardText(HWND owner);
void setClipboardText(HWND owner, const std::wstring& text);
// The characters an Open dialog's path may take: the longest path Windows
// has, not MAX_PATH, past which the dialog returns as if it were cancelled
// and a file deep in its folders could not be picked at all.
inline constexpr DWORD kFileDialogPathChars = 32768;
// What Windows' Open or Save dialog is asked for.
struct FileDialogRequest {
    // The kinds of file it lists, as OPENFILENAMEW's lpstrFilter has them.
    const wchar_t* filter{};
    // A Save dialog's extension, put on a name typed without one.
    const wchar_t* extension{};
    // The name it starts with, and the folder it opens in: Windows' own
    // choice of folder where none is given.
    std::wstring name;
    std::wstring folder;
};
inline std::optional<std::wstring> fileDialog(HWND owner, const FileDialogRequest& request, bool save) {
    std::vector<wchar_t> path(kFileDialogPathChars);
    request.name.copy(path.data(), path.size() - 1);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = request.filter;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrInitialDir = request.folder.empty() ? nullptr : request.folder.c_str();
    dialog.lpstrDefExt = request.extension;
    dialog.Flags = OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog))) return std::nullopt;
    return std::wstring(path.data());
}
// The file picked in the Open dialog, one that exists; none when the dialog
// was cancelled.
inline std::optional<std::wstring> openFileDialog(HWND owner, const FileDialogRequest& request) {
    return fileDialog(owner, request, false);
}
// The file named in the Save dialog, in a folder that exists, once the
// viewer has said to replace one already there; none when cancelled.
inline std::optional<std::wstring> saveFileDialog(HWND owner, const FileDialogRequest& request) {
    return fileDialog(owner, request, true);
}
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
// A hot control shows its name after resting on it this long, in the
// player's window and the settings window alike.
inline constexpr ULONGLONG kTooltipDelayMs = 600;
// A step the list beside the video follows (App::followStepInList) waits
// this long for the video to change and for the list to show the new one --
// a folder being read, the server's reply -- and is then given up.
inline constexpr ULONGLONG kListFollowWaitMs = 5000;

// Per-monitor DPI without a minimum SDK: both calls exist since Windows 10
// 1607, and a build without them reports 96, which keeps the 1x layout.
inline unsigned windowDpi(HWND window) {
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
    using GetDpiForSystemFn = UINT(WINAPI*)();
    if (user32 && window) {
        if (const auto fn = reinterpret_cast<GetDpiForWindowFn>(
                GetProcAddress(user32, "GetDpiForWindow"))) {
            if (const UINT dpi = fn(window)) return dpi;
        }
    }
    if (user32) {
        if (const auto fn = reinterpret_cast<GetDpiForSystemFn>(
                GetProcAddress(user32, "GetDpiForSystem"))) {
            if (const UINT dpi = fn()) return dpi;
        }
    }
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<unsigned>(dpi) : 96U;
}

// ---------------------------------------------------------------------------
// The player's own windows: the player and its video, the settings window
// and its drop-down list, the delete dialog.

// The App a window belongs to: put in the window's user data at
// WM_NCCREATE, from CreateWindowExW's parameter, and read back from there
// after. Null for the messages before WM_NCCREATE.
inline App* windowApp(HWND window, UINT message, LPARAM lParam) {
    if (message != WM_NCCREATE) return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    auto* app = static_cast<App*>(reinterpret_cast<const CREATESTRUCTW*>(lParam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    return app;
}

// Registered once, with the arrow for a cursor. With no icon Windows gives
// the default one; with one, it picks the small frame from the icon's own
// resource group rather than shrinking the large bitmap.
inline bool registerWindowClass(HINSTANCE instance, const wchar_t* name, WNDPROC procedure, UINT style,
                                HBRUSH background, HICON icon = nullptr) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = style;
    windowClass.lpfnWndProc = procedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = name;
    windowClass.hIcon = icon;
    return RegisterClassExW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

// For a window just made, still hidden: its frame worked out again through
// WM_NCCALCSIZE, so Windows and DWM drop the caption it does not draw, and
// the frame in Windows' dark colours (DWMWA_USE_IMMERSIVE_DARK_MODE, 20).
// Shown first, it would keep the light frame it was painted with.
inline void darkCaptionlessFrame(HWND window) {
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    const BOOL darkTitle = TRUE;
    const DWORD darkTitleAttribute = 20;
    DwmSetWindowAttribute(window, darkTitleAttribute, &darkTitle, sizeof(darkTitle));
}

// The rectangle WM_NCCALCSIZE turns from the window's into its client's, in
// either form of the message: CreateWindowExW sends the one with a bare
// RECT, and a window that never changes size afterwards keeps what it said.
inline RECT* ncCalcSizeClient(WPARAM wParam, LPARAM lParam) {
    return wParam ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0] : reinterpret_cast<RECT*>(lParam);
}

// WM_NCCALCSIZE for a window with no title bar: Windows keeps its own frame
// on the sides and the bottom -- the resize borders, the shadow, snapping --
// and the client reaches the window's top edge, where the window draws its
// own caption.
inline LRESULT captionlessClientArea(HWND window, WPARAM wParam, LPARAM lParam) {
    RECT* client = ncCalcSizeClient(wParam, lParam);
    const LONG top = client->top;
    const LRESULT result = DefWindowProcW(window, WM_NCCALCSIZE, wParam, lParam);
    if (result == 0) client->top = top;
    return result;
}

// WM_DPICHANGED for a window that goes where Windows suggests: the scale
// first, as the move paints the window at once, then the rectangle asked.
inline void followDpiChange(HWND window, WPARAM wParam, LPARAM lParam, float& scale) {
    scale = static_cast<float>(HIWORD(wParam)) / 96.0F;
    if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
        SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

// Asks for one WM_MOUSELEAVE once the pointer is over the window; the
// handler of that message clears `tracking` for the next.
inline void trackMouseLeave(HWND window, bool& tracking) {
    if (tracking) return;
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
    tracking = TrackMouseEvent(&track) != FALSE;
}

// WM_PAINT for a window its swap chain paints whole: what Windows asks to
// be painted is marked done, and the caller draws.
inline void validatePaint(HWND window) {
    PAINTSTRUCT paint{};
    BeginPaint(window, &paint);
    EndPaint(window, &paint);
}

inline OverlayRect overlayRect(const RECT& rect) {
    return {static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
            static_cast<float>(rect.bottom - rect.top)};
}

// The resize border Windows gives a window at this DPI, the band at the top
// of a captionless client that resizes it, and what a maximized window
// hangs off the monitor.
inline int resizeFrameThickness(unsigned dpi) {
    using GetSystemMetricsForDpiFn = int(WINAPI*)(int, UINT);
    static const auto forDpi = [] {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        return user32 ? reinterpret_cast<GetSystemMetricsForDpiFn>(
                            GetProcAddress(user32, "GetSystemMetricsForDpi"))
                      : nullptr;
    }();
    if (forDpi && dpi > 0) {
        return forDpi(SM_CYSIZEFRAME, dpi) + forDpi(SM_CXPADDEDBORDER, dpi);
    }
    return GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
}

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
    CmdVideoSets, CmdAddVideoSet,
    CmdFullscreen, CmdSettings,
    CmdOpenEmby, CmdSubtitleOff,
    CmdSubtitleCycle, CmdSubtitleLoad,
    CmdSubtitleEarlier, CmdSubtitleLater, CmdSubtitleSyncReset,
    CmdPlayOrderFirst, CmdPlayOrderLast = CmdPlayOrderFirst + kPlayOrderCount - 1,
    CmdSubtitleFirst, CmdSubtitleLast = CmdSubtitleFirst + 63,
    CmdAudioTrackFirst, CmdAudioTrackLast = CmdAudioTrackFirst + 31,
    // An Emby item marked or unmarked as a favorite: a pane's, or the
    // browser item a right-click was on.
    CmdEmbyFavorite,
    // O's Open dialog, from the menu away from any pane.
    CmdOpenFiles,
    // The sound (AppAudio.cpp): the output -- Windows' default first, then
    // each listed, the last place the chosen one when it is not connected --
    // and its speakers; the effects; a pane's timing; the Audio tab.
    CmdAudioOutputFirst, CmdAudioOutputLast = CmdAudioOutputFirst + 31,
    CmdAudioSpeakersStereo, CmdAudioSpeakersDevice,
    CmdAudioEqualizer,
    CmdEqualizerPresetFirst, CmdEqualizerPresetLast = CmdEqualizerPresetFirst + static_cast<unsigned>(kEqualizerPresets.size()) - 1,
    CmdAudioNormalize,
    CmdAudioNightFirst, CmdAudioNightLast = CmdAudioNightFirst + kAudioStrengthCount - 1,
    CmdAudioStereoFirst, CmdAudioStereoLast = CmdAudioStereoFirst + kAudioStereoModeCount - 1,
    CmdAudioVoiceEmphasis, CmdAudioCrossfeed,
    CmdAudioWideningFirst, CmdAudioWideningLast = CmdAudioWideningFirst + kAudioStrengthCount - 1,
    CmdAudioPitch,
    CmdAudioSettings,
    CmdAudioEarlier, CmdAudioLater, CmdAudioDelayReset,
    // The picture's way to the screen (AppVideo.cpp): the renderer, the
    // deinterlacing, the scaler, the colour controls' reset; the Video tab.
    CmdVideoRendererFirst, CmdVideoRendererLast = CmdVideoRendererFirst + kVideoRendererCount - 1,
    CmdVideoDeinterlace,
    CmdVideoScalingFirst, CmdVideoScalingLast = CmdVideoScalingFirst + kVideoScalingCount - 1,
    CmdVideoColorReset,
    CmdVideoSettings,
    // The browser item a right-click was on, or a pane's video, deleted
    // from the server.
    CmdEmbyDelete,
    // A text box's right-click menu (AppTextFields.cpp), and a spin box's
    // in the settings window.
    CmdFieldCut, CmdFieldCopy, CmdFieldPaste, CmdFieldDelete, CmdFieldSelectAll
};
inline constexpr std::size_t kAudioOutputMenuLimit = CmdAudioOutputLast - CmdAudioOutputFirst + 1;

// A text box's right-click menu, Windows' edit box's, its lines greyed
// where they would do nothing (textFieldCanEdit). `pasted` is the
// clipboard's text as the box takes it, `limit` how much the box holds.
inline PanelMenu fieldEditMenu(const TextFieldState& field, bool masked, std::wstring_view pasted,
                               std::size_t limit = kTextFieldLimit) {
    const auto can = [&](TextFieldEdit edit) { return textFieldCanEdit(field, edit, masked, pasted, limit); };
    PanelMenu menu;
    menu.option(CmdFieldCut, L"Cut", false, L"Ctrl+X", can(TextFieldEdit::Cut));
    menu.option(CmdFieldCopy, L"Copy", false, L"Ctrl+C", can(TextFieldEdit::Copy));
    menu.option(CmdFieldPaste, L"Paste", false, L"Ctrl+V", can(TextFieldEdit::Paste));
    menu.option(CmdFieldDelete, L"Delete", false, {}, can(TextFieldEdit::Delete));
    menu.separator();
    menu.option(CmdFieldSelectAll, L"Select all", false, L"Ctrl+A", can(TextFieldEdit::SelectAll));
    return menu;
}

// The edit a line of that menu is.
inline std::optional<TextFieldEdit> fieldMenuEdit(unsigned command) {
    switch (command) {
    case CmdFieldCut: return TextFieldEdit::Cut;
    case CmdFieldCopy: return TextFieldEdit::Copy;
    case CmdFieldPaste: return TextFieldEdit::Paste;
    case CmdFieldDelete: return TextFieldEdit::Delete;
    case CmdFieldSelectAll: return TextFieldEdit::SelectAll;
    default: return std::nullopt;
    }
}

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

// What the audio-track submenu shows for one stream.
inline std::wstring audioTrackLabel(const AudioTrackInfo& track) {
    std::wstring label;
    if (!track.title.empty()) label = utf8ToWideText(track.title);
    if (!track.language.empty()) {
        if (!label.empty()) label += L"  ";
        label += utf8ToWideText(track.language);
    }
    if (label.empty()) label = L"Track " + std::to_wstring(track.streamIndex);
    std::wstring detail = utf8ToWideText(track.codec);
    if (track.channels > 0) {
        if (!detail.empty()) detail += L' ';
        detail += std::to_wstring(track.channels) + L"ch";
    }
    if (!detail.empty()) label += L"  (" + detail + L")";
    return label;
}

// A level as the settings show it, up to 200%: the master volume may be
// raised past 100%.
inline std::wstring percent(float value) {
    return std::to_wstring(static_cast<int>(std::lround(std::clamp(value, 0.0F, 2.0F) * 100.0F))) + L"%";
}

// A sound delay as the Audio tab and the notices show it: "+150 ms",
// "−50 ms", "±0 ms".
inline std::wstring audioDelayText(double seconds) {
    const long long ms = std::llround(seconds * 1000.0);
    std::wostringstream text;
    text << (ms > 0 ? L"+" : ms < 0 ? L"\x2212" : L"\x00B1") << std::llabs(ms) << L" ms";
    return text.str();
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
