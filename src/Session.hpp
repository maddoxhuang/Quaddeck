#pragma once

#include "Core.hpp"
#include "EmbyLibraryPrefs.hpp"

#include <algorithm>
#include <array>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <utility>

namespace quaddeck {

struct PaneSession {
    std::wstring path;
    double delay{};
    double adjustment{};
    double rate{1.0};
    bool paused{};
    double pausedTime{};
    bool repeat{};
    bool loopEnabled{};
    double loopA{};
    double loopB{};
    PaneView view{};
    float volume{1.0F};
    bool muted{};
};

struct SessionState {
    double timeline{};
    bool playing{};
    LayoutMode layout{LayoutMode::Grid2x2};
    unsigned audioMask{1};
    DecodeMode decode{DecodeMode::Automatic};
    ShaderPreset shader{ShaderPreset::Normal};
    SeekMode seek{SeekMode::Linked};
    bool controlsVisible{true};
    bool repeatAll{true};
    int expandedPane{-1};
    bool masterLoopEnabled{};
    double masterLoopA{};
    double masterLoopB{};
    std::wstring customShaderPath;
    SmartVibranceSettings smartVibrance;
    PaneArray<PaneSession> panes{};
};

// A visual preset deliberately contains no media paths or playback position.
// It can be applied to the videos that are currently open, and also seeds the
// view settings of empty panes for videos loaded later.
struct StyleState {
    LayoutMode layout{LayoutMode::Grid2x2};
    int expandedPane{-1};
    ShaderPreset shader{ShaderPreset::Normal};
    bool controlsPinned{};
    std::wstring customShaderPath;
    SmartVibranceSettings smartVibrance;
    PaneArray<PaneView> paneViews{};
};

struct AppSettings {
    bool nasCache{true};
    // Device features rather than a look, so they belong to the machine's
    // settings and not to a visual preset.
    VideoEnhancementSettings rtxVideo;
    EmbyBrowserPrefs embyBrowser;
    EmbyLibraryPrefs embyLibraries;
    PlayOrder playOrder{PlayOrder::InOrder};
    bool keyframeSeek{true};
    SubtitleSettings subtitles;
    StyleState style;
    DecodeMode decode{DecodeMode::Automatic};
    SeekMode seek{SeekMode::Linked};
    bool repeatAll{true};
    unsigned audioMask{1};
    PaneArray<bool> paneRepeat{};
    float volume{1.0F};
    bool muted{};
    PaneArray<float> paneVolume{1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    PaneArray<bool> paneMuted{};
    int windowX{};
    int windowY{};
    int windowWidth{1440};
    int windowHeight{900};
    bool maximized{};
};

inline bool writeStyle(std::wostream& output, const StyleState& state) {
    const auto vibrance = clampSmartVibranceSettings(state.smartVibrance);
    output << L"QSTYLE 3\n";
    output << L"settings " << static_cast<int>(state.layout) << L' '
           << state.expandedPane << L' ' << static_cast<int>(state.shader) << L' '
           << state.controlsPinned << L'\n';
    output << L"shaderpath " << std::quoted(state.customShaderPath) << L'\n';
    output << L"smartvibrance " << vibrance.intensity << L' '
           << vibrance.saturationPivot << L' ' << vibrance.grayPivot << L' '
           << vibrance.graySharpness << L'\n';
    for (std::size_t index = 0; index < state.paneViews.size(); ++index) {
        output << L"pane " << index << L' '
               << static_cast<int>(state.paneViews[index].mode) << L' '
               << state.paneViews[index].zoom << L'\n';
    }
    return output.good();
}

inline bool readStyle(std::wistream& input, StyleState& state) {
    std::wstring key;
    int version{};
    int layout{};
    int shader{};
    if (!(input >> key >> version) || key != L"QSTYLE" ||
        (version < 1 || version > 3)) return false;
    if (!(input >> key) || key != L"settings" ||
        !(input >> layout >> state.expandedPane >> shader >> state.controlsPinned)) return false;
    if (!(input >> key) || key != L"shaderpath" ||
        !(input >> std::quoted(state.customShaderPath))) return false;
    if (version >= 2) {
        if (!(input >> key) || key != L"smartvibrance" ||
            !(input >> state.smartVibrance.intensity >> state.smartVibrance.saturationPivot >>
              state.smartVibrance.grayPivot >> state.smartVibrance.graySharpness) ||
            !finiteSmartVibranceSettings(state.smartVibrance)) return false;
        state.smartVibrance = clampSmartVibranceSettings(state.smartVibrance);
    }
    state.layout = static_cast<LayoutMode>(std::clamp(layout, 0, 5));
    state.expandedPane = std::clamp(
        state.expandedPane, -1, static_cast<int>(kMaxPanes - 1));
    state.shader = static_cast<ShaderPreset>(std::clamp(shader, 0, 4));
    const std::size_t paneCount = version >= 3 ? kMaxPanes : kLegacyPaneCount;
    for (std::size_t index = paneCount; index < state.paneViews.size(); ++index) {
        state.paneViews[index] = PaneView{};
    }
    for (std::size_t expected = 0; expected < paneCount; ++expected) {
        std::size_t index{};
        int mode{};
        float zoom{};
        if (!(input >> key) || key != L"pane" ||
            !(input >> index >> mode >> zoom) || index != expected || !std::isfinite(zoom)) {
            return false;
        }
        state.paneViews[index].mode = static_cast<ViewMode>(std::clamp(mode, 0, 2));
        state.paneViews[index].zoom = std::clamp(zoom, 1.0F, 4.0F);
    }
    return true;
}

inline bool writeAppSettings(std::wostream& output, const AppSettings& state) {
    if (!validateEmbyLibraryPrefs(state.embyLibraries)) return false;
    output << L"QCONFIG 14\n";
    output << L"playback " << static_cast<int>(state.decode) << L' '
           << static_cast<int>(state.seek) << L' ' << state.repeatAll << L' '
           << (state.audioMask & kAllPaneMask) << L' ' << state.volume << L' ' << state.muted << L'\n';
    output << L"window " << state.windowX << L' ' << state.windowY << L' '
           << state.windowWidth << L' ' << state.windowHeight << L' '
           << state.maximized << L'\n';
    output << L"repeatpane";
    for (bool repeat : state.paneRepeat) output << L' ' << repeat;
    output << L'\n';
    output << L"panevolume";
    for (std::size_t index = 0; index < state.paneVolume.size(); ++index) {
        output << L' ' << std::clamp(state.paneVolume[index], 0.0F, 1.0F)
               << L' ' << state.paneMuted[index];
    }
    output << L'\n';
    output << L"nascache " << state.nasCache << L'\n';
    output << L"rtxvideo " << state.rtxVideo.superResolution << L' '
           << state.rtxVideo.rtxHdr << L'\n';
    const auto browser = clampEmbyBrowserPrefs(state.embyBrowser);
    output << L"embybrowser " << browser.view << L' ' << browser.sort << L' ' << browser.descending
           << L' ' << browser.unplayed << L' ' << browser.flat << L'\n';
    output << L"playorder " << static_cast<int>(state.playOrder) << L'\n';
    output << L"keyframeseek " << state.keyframeSeek << L'\n';
    const auto subtitles = clampSubtitleSettings(state.subtitles);
    output << L"subtitles " << subtitles.show << L' ' << subtitles.language << L' '
           << subtitles.size << L' ' << subtitles.position << L' ' << subtitles.background << L'\n';
    auto libraries = state.embyLibraries.entries;
    std::sort(libraries.begin(), libraries.end(), [](const auto& a, const auto& b) {
        return a.serverId != b.serverId ? a.serverId < b.serverId : a.libraryId < b.libraryId;
    });
    output << L"embylibrarydetails " << libraries.size() << L'\n';
    for (const auto& library : libraries) {
        output << L"embylibrarydetail " << std::quoted(utf8ToWideText(library.serverId)) << L' '
               << std::quoted(utf8ToWideText(library.libraryId)) << L' '
               << static_cast<int>(library.detailsEnabled) << L'\n';
    }
    return writeStyle(output, state.style);
}

inline bool readAppSettings(std::wistream& input, AppSettings& state) {
    std::wstring key;
    int version{};
    int decode{};
    int seek{};
    unsigned audioValue{};
    if (!(input >> key >> version) || key != L"QCONFIG" ||
        (version < 1 || version > 14)) return false;
    state.nasCache = true;
    state.subtitles = SubtitleSettings{};
    state.rtxVideo = VideoEnhancementSettings{};
    state.embyBrowser = EmbyBrowserPrefs{};
    state.embyLibraries = EmbyLibraryPrefs{};
    state.playOrder = PlayOrder::InOrder;
    state.keyframeSeek = true;
    state.paneRepeat.fill(false);
    state.paneVolume.fill(1.0F);
    state.paneMuted.fill(false);
    if (!(input >> key) || key != L"playback" ||
        !(input >> decode >> seek >> state.repeatAll >> audioValue >>
          state.volume >> state.muted)) return false;
    if (!(input >> key) || key != L"window" ||
        !(input >> state.windowX >> state.windowY >> state.windowWidth >>
          state.windowHeight >> state.maximized)) return false;
    if (version >= 2) {
        if (!(input >> key) || key != L"repeatpane") return false;
        const std::size_t paneCount = version >= 5 ? kMaxPanes : kLegacyPaneCount;
        state.paneRepeat.fill(false);
        for (std::size_t index = 0; index < paneCount; ++index) {
            bool value{};
            if (!(input >> value)) return false;
            state.paneRepeat[index] = value;
        }
    }
    if (version >= 4) {
        if (!(input >> key) || key != L"panevolume") return false;
        const std::size_t paneCount = version >= 5 ? kMaxPanes : kLegacyPaneCount;
        state.paneVolume.fill(1.0F);
        state.paneMuted.fill(false);
        for (std::size_t index = 0; index < paneCount; ++index) {
            float volume{};
            bool muted{};
            if (!(input >> volume >> muted) || !std::isfinite(volume)) return false;
            state.paneVolume[index] = std::clamp(volume, 0.0F, 1.0F);
            state.paneMuted[index] = muted;
        }
    }
    if (version >= 6 && (!(input >> key) || key != L"nascache" || !(input >> state.nasCache)))
        return false;
    // Older files never had the RTX Video line; both features stay off, which
    // is also what a first run gets.
    if (version >= 7 && (!(input >> key) || key != L"rtxvideo" ||
                         !(input >> state.rtxVideo.superResolution >> state.rtxVideo.rtxHdr)))
        return false;
    // Older files predate the browser's layout choices; thumbnails by name.
    if (version >= 8 && (!(input >> key) || key != L"embybrowser" ||
                         !(input >> state.embyBrowser.view >> state.embyBrowser.sort >>
                           state.embyBrowser.descending >> state.embyBrowser.unplayed >>
                           state.embyBrowser.flat)))
        return false;
    state.embyBrowser = clampEmbyBrowserPrefs(state.embyBrowser);
    // Older files predate the playback order; a single video plays on in order.
    if (version >= 9) {
        int order{};
        if (!(input >> key) || key != L"playorder" || !(input >> order)) return false;
        state.playOrder = clampPlayOrder(order);
    }
    // Older files predate the choice; one video seeks to keyframes.
    if (version >= 10 && (!(input >> key) || key != L"keyframeseek" || !(input >> state.keyframeSeek)))
        return false;
    // Older files predate the subtitle choices: shown, in the language
    // Windows names, at the default size and place.
    if (version >= 11 && (!(input >> key) || key != L"subtitles" ||
                          !(input >> state.subtitles.show >> state.subtitles.language >>
                            state.subtitles.size >> state.subtitles.position)))
        return false;
    // QCONFIG 11 drew a dark box behind every line and had no word for it;
    // the line gained one with 12, and an 11 is read as without the box.
    if (version >= 12 && !(input >> state.subtitles.background)) return false;
    // Before 14 the position was the height of every bottom line above the
    // pane's edge; it is now how far they are raised above their script's
    // own place, and an old value means nothing as that.
    if (version < 14) state.subtitles.position = SubtitleSettings{}.position;
    state.subtitles = clampSubtitleSettings(state.subtitles);
    if (version >= 13) {
        long long count{};
        if (!(input >> key >> count) || key != L"embylibrarydetails" || count < 0 ||
            count > static_cast<long long>(kMaxEmbyLibraryPreferences)) return false;
        const auto readId = [&](std::string& id) {
            std::wstring wide;
            input >> std::ws;
            if (input.peek() != L'"' || !(input >> std::quoted(wide))) return false;
            id = wideToUtf8Text(wide);
            return utf8ToWideText(id) == wide && validEmbyLibraryPreferenceId(id);
        };
        EmbyLibraryPrefs libraries;
        for (long long index = 0; index < count; ++index) {
            EmbyLibraryPreference library;
            int enabled{};
            if (!(input >> key) || key != L"embylibrarydetail" ||
                !readId(library.serverId) || !readId(library.libraryId) ||
                !(input >> enabled) || (enabled != 0 && enabled != 1)) return false;
            library.detailsEnabled = enabled == 1;
            libraries.entries.push_back(std::move(library));
        }
        if (!validateEmbyLibraryPrefs(libraries)) return false;
        state.embyLibraries = std::move(libraries);
    }
    if (!readStyle(input, state.style) || !std::isfinite(state.volume)) return false;
    state.decode = static_cast<DecodeMode>(std::clamp(decode, 0, 2));
    state.seek = static_cast<SeekMode>(std::clamp(seek, 0, 1));
    const unsigned readableMask = version >= 5 ? kAllPaneMask : 0x0FU;
    state.audioMask = version >= 2 ? (audioValue & readableMask)
                                   : (1U << std::min(audioValue, 3U));
    state.volume = std::clamp(state.volume, 0.0F, 1.0F);
    state.windowWidth = std::clamp(state.windowWidth, 640, 10000);
    state.windowHeight = std::clamp(state.windowHeight, 400, 10000);
    state.windowX = std::clamp(state.windowX, -50000, 50000);
    state.windowY = std::clamp(state.windowY, -50000, 50000);
    return true;
}

inline bool writeSession(std::wostream& output, const SessionState& state) {
    const auto vibrance = clampSmartVibranceSettings(state.smartVibrance);
    output << L"QDECK 6\n";
    output << L"timeline " << std::setprecision(17) << state.timeline << L' '
           << state.playing << L'\n';
    output << L"settings " << static_cast<int>(state.layout) << L' ' << (state.audioMask & kAllPaneMask) << L' '
           << static_cast<int>(state.decode) << L' ' << static_cast<int>(state.shader) << L' '
           << static_cast<int>(state.seek) << L' ' << state.controlsVisible << L' '
           << state.repeatAll << L' ' << state.expandedPane << L'\n';
    output << L"masterloop " << state.masterLoopEnabled << L' ' << state.masterLoopA << L' '
           << state.masterLoopB << L'\n';
    output << L"shaderpath " << std::quoted(state.customShaderPath) << L'\n';
    output << L"smartvibrance " << vibrance.intensity << L' '
           << vibrance.saturationPivot << L' ' << vibrance.grayPivot << L' '
           << vibrance.graySharpness << L'\n';
    for (std::size_t index = 0; index < state.panes.size(); ++index) {
        const auto& pane = state.panes[index];
        output << L"pane " << index << L' ' << std::quoted(pane.path) << L' '
               << pane.delay << L' ' << pane.adjustment << L' ' << pane.rate << L' '
               << pane.paused << L' ' << pane.pausedTime << L' '
               << pane.repeat << L' '
               << pane.loopEnabled << L' ' << pane.loopA << L' ' << pane.loopB << L' '
               << static_cast<int>(pane.view.mode) << L' ' << pane.view.zoom << L' '
               << std::clamp(pane.volume, 0.0F, 1.0F) << L' ' << pane.muted << L'\n';
    }
    return output.good();
}

inline bool readSession(std::wistream& input, SessionState& state) {
    std::wstring key;
    int version{};
    if (!(input >> key >> version) || key != L"QDECK" ||
        (version < 1 || version > 6)) return false;
    int layout{}, decode{}, shader{}, seek{};
    unsigned audioValue{};
    if (!(input >> key) || key != L"timeline" || !(input >> state.timeline >> state.playing)) return false;
    if (!(input >> key) || key != L"settings" ||
        !(input >> layout >> audioValue >> decode >> shader >> seek >> state.controlsVisible)) return false;
    if (version >= 2 && !(input >> state.repeatAll >> state.expandedPane)) return false;
    if (!(input >> key) || key != L"masterloop" ||
        !(input >> state.masterLoopEnabled >> state.masterLoopA >> state.masterLoopB)) return false;
    if (!(input >> key) || key != L"shaderpath" || !(input >> std::quoted(state.customShaderPath))) return false;
    if (version >= 4) {
        if (!(input >> key) || key != L"smartvibrance" ||
            !(input >> state.smartVibrance.intensity >> state.smartVibrance.saturationPivot >>
              state.smartVibrance.grayPivot >> state.smartVibrance.graySharpness) ||
            !finiteSmartVibranceSettings(state.smartVibrance)) return false;
        state.smartVibrance = clampSmartVibranceSettings(state.smartVibrance);
    }
    state.layout = static_cast<LayoutMode>(std::clamp(layout, 0, 5));
    const unsigned readableMask = version >= 6 ? kAllPaneMask : 0x0FU;
    state.audioMask = version >= 3 ? (audioValue & readableMask)
                                   : (1U << std::min(audioValue, 3U));
    state.decode = static_cast<DecodeMode>(std::clamp(decode, 0, 2));
    state.shader = static_cast<ShaderPreset>(std::clamp(shader, 0, 4));
    state.seek = static_cast<SeekMode>(std::clamp(seek, 0, 1));
    state.expandedPane = std::clamp(
        state.expandedPane, -1, static_cast<int>(kMaxPanes - 1));
    if (!std::isfinite(state.timeline) || !std::isfinite(state.masterLoopA) ||
        !std::isfinite(state.masterLoopB)) return false;
    state.timeline = std::max(0.0, state.timeline);
    const std::size_t paneCount = version >= 6 ? kMaxPanes : kLegacyPaneCount;
    for (std::size_t index = paneCount; index < state.panes.size(); ++index) {
        state.panes[index] = PaneSession{};
    }
    for (std::size_t expected = 0; expected < paneCount; ++expected) {
        std::size_t index{};
        int viewMode{};
        PaneSession pane;
        if (!(input >> key) || key != L"pane" ||
            !(input >> index >> std::quoted(pane.path) >> pane.delay >> pane.adjustment >> pane.rate >>
              pane.paused >> pane.pausedTime) || index != expected) return false;
        if (version >= 3 && !(input >> pane.repeat)) return false;
        if (!(input >> pane.loopEnabled >> pane.loopA >> pane.loopB >>
              viewMode >> pane.view.zoom)) return false;
        if (version >= 5 && !(input >> pane.volume >> pane.muted)) return false;
        if (!std::isfinite(pane.delay) || !std::isfinite(pane.adjustment) ||
            !std::isfinite(pane.rate) || !std::isfinite(pane.pausedTime) ||
            !std::isfinite(pane.loopA) || !std::isfinite(pane.loopB) ||
            !std::isfinite(pane.view.zoom) || !std::isfinite(pane.volume)) return false;
        pane.delay = std::clamp(pane.delay, 0.0, 86400.0);
        pane.rate = std::clamp(pane.rate, 0.25, 4.0);
        pane.pausedTime = std::max(0.0, pane.pausedTime);
        pane.view.mode = static_cast<ViewMode>(std::clamp(viewMode, 0, 2));
        pane.view.zoom = std::clamp(pane.view.zoom, 1.0F, 4.0F);
        pane.volume = std::clamp(pane.volume, 0.0F, 1.0F);
        // An enabled loop needs only a usable A; an unset B means "to the end".
        if (!std::isfinite(pane.loopA) || pane.loopA < 0.0) pane.loopEnabled = false;
        state.panes[index] = std::move(pane);
    }
    if (!std::isfinite(state.masterLoopA) || state.masterLoopA < 0.0) state.masterLoopEnabled = false;
    return true;
}

}  // namespace quaddeck
