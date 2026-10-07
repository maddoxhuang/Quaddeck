// App: sessions, visual styles and the per-user settings file -- capturing
// App state into the Session.hpp formats and applying it back.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"
#include "resource.h"

#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <utility>

namespace quaddeck {

using namespace app_internal;

SessionState App::captureSession() const {
    SessionState state;
    state.timeline = deviceRecoveryPending_
        ? deviceRecoveryPosition_ : clock_.position();
    state.playing = playbackIntended();
    state.layout = layoutMode_;
    state.audioMask = audioMask_;
    state.decode = decodeMode_;
    state.shader = shaderPreset_;
    state.seek = seekMode_;
    state.controlsVisible = controlsPinned_;
    state.repeatAll = repeatAll_;
    state.expandedPane = expandedPane_;
    state.masterLoopEnabled = masterLoopEnabled_;
    state.masterLoopA = masterLoopA_;
    state.masterLoopB = masterLoopB_;
    state.customShaderPath = customShaderPath_;
    state.smartVibrance = smartVibrance_;
    for (std::size_t pane = 0; pane < state.panes.size(); ++pane) {
        auto& target = state.panes[pane];
        target.path = paths_[pane];
        target.delay = startDelays_[pane];
        target.adjustment = syncAdjustments_[pane];
        target.rate = playbackRates_[pane];
        target.paused = sourcePaused_[pane];
        target.pausedTime = sourcePausedTimes_[pane];
        target.loopEnabled = sourceLoopEnabled_[pane];
        target.loopA = sourceLoopA_[pane];
        target.loopB = sourceLoopB_[pane];
        target.repeat = sourceAutoRepeat_[pane];
        target.view = paneViews_[pane];
        target.volume = audio_.paneVolume(pane);
        target.muted = audio_.paneMuted(pane);
    }
    return state;
}

bool App::saveSession(const std::wstring& path) {
    const SessionState state = captureSession();
    if (!writeUtf8FileAtomically(
            std::filesystem::path(path),
            [&](std::wostream& output) { return writeSession(output, state); })) {
        return false;
    }
    sessionPath_ = path;
    return true;
}

bool App::loadSession(const std::wstring& path) {
    std::wstring text;
    if (!readUtf8File(std::filesystem::path(path), text)) return false;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    SessionState state;
    if (!readSession(input, state)) return false;
    applySession(state);
    sessionPath_ = path;
    return true;
}

void App::saveSessionDialog() {
    wchar_t path[MAX_PATH]{};
    if (!sessionPath_.empty()) wcsncpy_s(path, sessionPath_.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"QuadDeck session (*.qdeck)\0*.qdeck\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"qdeck";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&dialog) && !saveSession(path)) {
        MessageBoxW(window_, L"Could not save the QuadDeck session.", L"Session error", MB_ICONERROR);
    }
}

void App::loadSessionDialog() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"QuadDeck session (*.qdeck)\0*.qdeck\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) && !loadSession(path)) {
        MessageBoxW(window_, L"This file is not a valid QuadDeck session.", L"Session error", MB_ICONERROR);
    }
}

StyleState App::captureStyle() const {
    StyleState state;
    state.layout = layoutMode_;
    state.expandedPane = expandedPane_;
    state.shader = shaderPreset_;
    state.controlsPinned = controlsPinned_;
    state.customShaderPath = customShaderPath_;
    state.smartVibrance = smartVibrance_;
    state.paneViews = paneViews_;
    return state;
}

bool App::saveStyle(const std::wstring& path) {
    const StyleState state = captureStyle();
    return writeUtf8FileAtomically(
        std::filesystem::path(path),
        [&](std::wostream& output) { return writeStyle(output, state); });
}

bool App::applyStyle(const StyleState& state) {
    // Compile first. On failure, leave the current working shader and visual
    // state untouched rather than applying a half-loaded preset. During a
    // device retry there is deliberately no usable device; preserve the
    // selection and let the successful recovery compile it once.
    const bool deferShader = deviceRecoveryPending_ || renderer_.deviceLost();
    const bool shaderReady = deferShader || (state.customShaderPath.empty()
        ? renderer_.setShaderPreset(state.shader)
        : renderer_.loadPixelShader(state.customShaderPath));
    if (!shaderReady) return false;
    smartVibrance_ = clampSmartVibranceSettings(state.smartVibrance);
    renderer_.setSmartVibranceSettings(smartVibrance_);
    layoutMode_ = state.layout;
    expandedPane_ = state.expandedPane;
    soloPane_ = -1;
    shaderPreset_ = state.shader;
    customShaderPath_ = state.customShaderPath;
    controlsPinned_ = state.controlsPinned;
    controlsLastInteraction_ = GetTickCount64();
    paneViews_ = state.paneViews;
    setControlsVisible(true);
    layoutHoverControls();
    updateHoverControls();
    updateTitle();
    return true;
}

bool App::loadStyle(const std::wstring& path) {
    std::wstring text;
    if (!readUtf8File(std::filesystem::path(path), text)) return false;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    StyleState state;
    return readStyle(input, state) && applyStyle(state);
}

std::filesystem::path App::settingsPath() const {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &localAppData)) || !localAppData) {
        return {};
    }
    std::filesystem::path path(localAppData);
    CoTaskMemFree(localAppData);
    return path / L"QuadDeck" / L"settings.qconfig";
}

AppSettings App::captureAppSettings() const {
    AppSettings state;
    state.nasCache = nasCacheEnabled_;
    state.rtxVideo = rtxVideo_;
    state.embyBrowser = embyBrowser_;
    state.embyLibraries = embyLibraries_;
    state.playOrder = playOrder_;
    state.keyframeSeek = keyframeSeek_;
    state.subtitles = subtitleSettings_;
    state.style = captureStyle();
    state.decode = decodeMode_;
    state.seek = seekMode_;
    state.repeatAll = repeatAll_;
    state.audioMask = audioMask_;
    state.paneRepeat = sourceAutoRepeat_;
    state.volume = audio_.volume();
    state.muted = audio_.muted();
    for (std::size_t pane = 0; pane < state.paneVolume.size(); ++pane) {
        state.paneVolume[pane] = audio_.paneVolume(pane);
        state.paneMuted[pane] = audio_.paneMuted(pane);
    }
    WINDOWPLACEMENT placement{sizeof(placement)};
    if (fullscreen_) {
        placement = previousPlacement_;
    } else {
        GetWindowPlacement(window_, &placement);
    }
    state.windowX = placement.rcNormalPosition.left;
    state.windowY = placement.rcNormalPosition.top;
    state.windowWidth = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
    state.windowHeight = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
    state.maximized = placement.showCmd == SW_SHOWMAXIMIZED ||
                      (placement.flags & WPF_RESTORETOMAXIMIZED) != 0;
    return state;
}

bool App::saveAppSettings() const {
    const auto path = settingsPath();
    if (path.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const AppSettings state = captureAppSettings();
    return writeUtf8FileAtomically(
        path, [&](std::wostream& output) { return writeAppSettings(output, state); });
}

void App::scheduleAppSettingsSave() {
    settingsSavePending_ = true;
    settingsSaveRetryCount_ = 0;
    if (window_ && SetTimer(window_, kSettingsSaveTimer, kSettingsSaveDelayMs, nullptr)) {
        return;
    }
    settingsSavePending_ = false;
    if (!saveAppSettings()) appendDiagnostic("Could not save application settings");
}

void App::flushScheduledAppSettingsSave() {
    if (window_) KillTimer(window_, kSettingsSaveTimer);
    if (!settingsSavePending_) return;
    if (saveAppSettings()) {
        settingsSavePending_ = false;
        settingsSaveRetryCount_ = 0;
        return;
    }
    if (window_ && settingsSaveRetryCount_ < kMaximumSettingsSaveRetries) {
        ++settingsSaveRetryCount_;
        appendDiagnostic("Could not save application settings; retry scheduled");
        if (SetTimer(window_, kSettingsSaveTimer,
                     kSettingsSaveRetryDelayMs, nullptr)) {
            return;
        }
    }
    settingsSavePending_ = false;
    settingsSaveRetryCount_ = 0;
    appendDiagnostic("Could not save application settings");
}

bool App::loadAppSettings() {
    const auto path = settingsPath();
    if (path.empty() || !std::filesystem::exists(path)) return false;
    std::wstring text;
    if (!readUtf8File(path, text)) return false;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    AppSettings state;
    if (!readAppSettings(input, state)) return false;

    nasCacheEnabled_ = state.nasCache;
    rtxVideo_ = state.rtxVideo;
    renderer_.setVideoEnhancements(rtxVideo_);
    embyBrowser_ = clampEmbyBrowserPrefs(state.embyBrowser);
    embyLibraries_ = state.embyLibraries;
    playOrder_ = state.playOrder;
    keyframeSeek_ = state.keyframeSeek;
    subtitleSettings_ = clampSubtitleSettings(state.subtitles);
    decodeMode_ = state.decode;
    seekMode_ = state.seek;
    repeatAll_ = state.repeatAll;
    audioMask_ = state.audioMask & kAllPaneMask;
    sourceAutoRepeat_ = state.paneRepeat;
    StyleState visual = state.style;
    if (!applyStyle(visual)) {
        // A moved/deleted custom shader must not prevent every other setting
        // from loading. Fall back to Normal while preserving the visual layout.
        visual.customShaderPath.clear();
        visual.shader = ShaderPreset::Normal;
        applyStyle(visual);
    }
    audio_.setVolume(state.volume);
    audio_.setMuted(state.muted);
    for (std::size_t pane = 0; pane < state.paneVolume.size(); ++pane) {
        audio_.setPaneVolume(pane, state.paneVolume[pane]);
        audio_.setPaneMuted(pane, state.paneMuted[pane]);
    }
    refreshAudioControls();

    RECT desired{state.windowX, state.windowY,
                 state.windowX + state.windowWidth, state.windowY + state.windowHeight};
    if (MonitorFromRect(&desired, MONITOR_DEFAULTTONULL)) {
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(window_, &placement);
        placement.rcNormalPosition = desired;
        placement.showCmd = state.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        SetWindowPlacement(window_, &placement);
    }
    updateControls();
    updateTitle();
    return true;
}

void App::saveStyleDialog() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"QuadDeck visual preset (*.qstyle)\0*.qstyle\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"qstyle";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&dialog) && !saveStyle(path)) {
        MessageBoxW(window_, L"Could not save this visual preset.", L"Preset error", MB_ICONERROR);
    }
}

void App::loadStyleDialog() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"QuadDeck visual preset (*.qstyle)\0*.qstyle\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog) && !loadStyle(path)) {
        std::string detail = renderer_.error();
        if (detail.empty()) detail = "The preset file is invalid.";
        MessageBoxA(window_, detail.c_str(), "Preset error", MB_ICONERROR);
    }
}

void App::applySession(const SessionState& state) {
    browserSeekAligned_ = false;
    deviceRecoverySourceDurations_.fill(0.0);
    cancelSeekBarrier();
    clock_.pause();
    // The old deck's server items stop at their last position before any
    // of the timing that maps it is replaced.
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) embyReport(pane, "stopped");
    // These are transient choices from the old deck, not part of QDECK.
    // In particular, an old Solo must not hide the newly restored layout.
    soloPane_ = -1;
    hoverPane_ = -1;
    lastPointerPane_ = -1;
    for (auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }
    audio_.flush();
    for (auto& source : sources_) source.reset();
    sourceInitialAlignmentPending_.fill(false);
    sourceProvisionalTargets_.fill(0.0);
    paths_.fill({});
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) resetPaneMediaState(pane);
    ShaderPreset loadedPreset = state.shader;
    std::wstring loadedShaderPath = state.customShaderPath;
    const bool deferShader = deviceRecoveryPending_ || renderer_.deviceLost();
    const bool shaderReady = deferShader || (loadedShaderPath.empty()
        ? renderer_.setShaderPreset(loadedPreset)
        : renderer_.loadPixelShader(loadedShaderPath));
    if (!shaderReady) {
        const std::string shaderError = renderer_.error();
        loadedPreset = ShaderPreset::Normal;
        loadedShaderPath.clear();
        renderer_.setShaderPreset(loadedPreset);
        MessageBoxA(window_, shaderError.c_str(),
                    "Session pixel shader could not be loaded; using Normal", MB_ICONWARNING);
    }
    smartVibrance_ = clampSmartVibranceSettings(state.smartVibrance);
    renderer_.setSmartVibranceSettings(smartVibrance_);
    layoutMode_ = state.layout;
    audioMask_ = state.audioMask & kAllPaneMask;
    decodeMode_ = state.decode;
    shaderPreset_ = loadedPreset;
    seekMode_ = state.seek;
    controlsPinned_ = state.controlsVisible;
    controlsVisible_ = true;
    controlsLastInteraction_ = GetTickCount64();
    repeatAll_ = state.repeatAll;
    expandedPane_ = state.expandedPane;
    masterLoopEnabled_ = state.masterLoopEnabled;
    masterLoopA_ = state.masterLoopA;
    masterLoopB_ = state.masterLoopB;
    customShaderPath_ = std::move(loadedShaderPath);
    for (std::size_t pane = 0; pane < state.panes.size(); ++pane) {
        const auto& source = state.panes[pane];
        paths_[pane] = source.path;
        startDelays_[pane] = source.delay;
        syncAdjustments_[pane] = source.adjustment;
        playbackRates_[pane] = source.rate;
        sourcePaused_[pane] = source.paused;
        sourcePausedTimes_[pane] = source.pausedTime;
        sourceLoopEnabled_[pane] = source.loopEnabled;
        sourceLoopA_[pane] = source.loopA;
        sourceLoopB_[pane] = source.loopB;
        sourceAutoRepeat_[pane] = source.repeat;
        paneViews_[pane] = source.view;
        audio_.setPaneVolume(pane, source.volume);
        audio_.setPaneMuted(pane, source.muted);
        lastVideoMappedTimes_[pane] = -1.0;
    }
    refreshAudioControls();
    if (deviceRecoveryPending_) {
        deviceRecoveryPosition_ = state.timeline;
        deviceRecoveryResume_ = state.playing;
    }
    clock_.seek(state.timeline);
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        if (!paths_[pane].empty()) openSource(pane, paths_[pane], false);
    }
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
    }
    RECT client{};
    GetClientRect(window_, &client);
    layoutControls(static_cast<unsigned>(client.right), static_cast<unsigned>(client.bottom));
    if (state.playing && !deviceRecoveryPending_) togglePlayback();
    else { updateControls(); updateTitle(); }
}

}  // namespace quaddeck
