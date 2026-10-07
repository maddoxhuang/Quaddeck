// App: the settings sheet. Fills the SettingsPanel rows from player state,
// turns hits on them into the same state changes the popup menu and keys
// make, and owns the sheet's open/close, scroll and slide.

#include "App.hpp"
#include "AppInternal.hpp"
#include "TextEncoding.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace quaddeck {

using namespace app_internal;

namespace {

constexpr float kPanelSlideMs = 200.0F;
constexpr float kPanelRailInset = 8.0F;
// Smart Vibrance Plus slider ranges, matching clampSmartVibranceSettings.
constexpr std::array<float, 4> kVibranceMin{0.0F, 0.2F, 0.0005F, 5.0F};
constexpr std::array<float, 4> kVibranceMax{3.0F, 1.0F, 0.01F, 80.0F};

std::wstring percent(float value) {
    return std::to_wstring(static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 100.0F))) + L"%";
}

std::wstring seconds(double value, int precision = 2) {
    std::wostringstream text;
    text << std::showpos << std::fixed << std::setprecision(precision) << value << L" s";
    return text.str();
}

}  // namespace

void App::toggleSettingsPanel() {
    // F5, the gear and Settings in the menu always address normal settings,
    // even when the shared sheet last showed Emby or still shows it now.
    if (settingsOpen_ && !embyBrowserOpen_) {
        closeSettingsPanel();
        return;
    }
    if (embyBrowserOpen_) {
        closeSettingsPanel();
        embyBrowserOpen_ = false;
    }
    settingsOpen_ = true;
    restoreSheetScroll();
    refreshFileTypesStatus();
    controlsLastInteraction_ = GetTickCount64();
    setControlsVisible(true);
}

void App::toggleEmbyBrowser() {
    if (settingsOpen_ && embyBrowserOpen_) {
        closeSettingsPanel();
        embyBrowserOpen_ = false;
        return;
    }
    // F6 follows the explicitly selected video, including in a mixed deck.
    const auto target = embyPlaybackTarget();
    if (!paths_[target].empty() && !emby::isLocator(paths_[target])) openLocalList();
    else embyOpenBrowser();
}

bool App::embyBrowserDocked() const {
    return embyBrowserOpen_ && anyPaneLoaded();
}

float App::embyDockWidth(float clientWidth) const {
    // A quarter of the window, but never so narrow that a tile does not fit.
    return std::min(std::max(0.0F, clientWidth), std::max(clientWidth * 0.25F, 320.0F * uiScale_));
}

float App::videoAreaWidth(float clientWidth) const {
    if (!settingsOpen_ || !embyBrowserDocked()) return clientWidth;
    return std::max(0.0F, clientWidth - embyDockWidth(clientWidth));
}

float& App::sheetScrollSlot() {
    if (!embyBrowserOpen_) return settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    if (browserSource_ == BrowserSource::Emby && embyDetails_) return embyDetails_->scroll;
    return browserSource_ == BrowserSource::Local ? localListScroll_ : embyListScroll_;
}

void App::rememberSheetScroll() { sheetScrollSlot() = settingsScroll_; }

void App::restoreSheetScroll() { settingsScroll_ = sheetScrollSlot(); }

// The header's switch: the browser from the settings, the settings from
// the browser or the folder list. Signed out, the Emby side is the sign-in
// dialog, as Ctrl+E.
void App::switchSheet() {
    if (!settingsOpen_) return;
    controlsLastInteraction_ = GetTickCount64();
    if (!embyBrowserOpen_) {
        embyOpenBrowser();
        return;
    }
    embyCancelReplacement();
    localCancelReplacement();
    rememberSheetScroll();
    embyBrowserOpen_ = false;
    restoreSheetScroll();
}

void App::selectSettingsTab(SettingsTab tab) {
    if (!settingsOpen_ || embyBrowserOpen_ || tab == settingsTab_) return;
    rememberSheetScroll();
    settingsTab_ = tab;
    restoreSheetScroll();
    if (tab == SettingsTab::General) refreshFileTypesStatus();
    controlsLastInteraction_ = GetTickCount64();
}

void App::closeSettingsPanel() {
    embyCancelReplacement();
    localCancelReplacement();
    if (!settingsOpen_) return;
    rememberSheetScroll();
    settingsOpen_ = false;
    if (controlDrag_ == ControlDrag::PanelSlider) endControlDrag(true);
    if (controlDrag_ == ControlDrag::PanelScroll || controlDrag_ == ControlDrag::PanelScrollbar) endControlDrag(false);
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    pressedPanelRow_ = pressedPanelPart_ = -1;
    // Pictures still queued for the browser are not worth the bandwidth
    // once a video is starting under it.
    embyImages_.clearPending();
    localThumbnails_.clearPending();
}

std::vector<PanelRow> App::buildSettingsRows(float clientWidth) const {
    std::vector<PanelRow> rows;
    const auto header = [&](const wchar_t* text) {
        PanelRow row;
        row.kind = PanelRowKind::Header;
        row.label = text;
        rows.push_back(std::move(row));
    };
    const auto note = [&](const std::wstring& text) {
        PanelRow row;
        row.kind = PanelRowKind::Note;
        row.label = text;
        rows.push_back(std::move(row));
    };
    const auto toggle = [&](SettingId id, int param, const std::wstring& label, bool on, bool enabled = true) {
        PanelRow row;
        row.kind = PanelRowKind::Toggle;
        row.id = id;
        row.param = param;
        row.label = label;
        row.on = on;
        row.enabled = enabled;
        rows.push_back(std::move(row));
    };
    const auto choice = [&](SettingId id, const std::wstring& label,
                            std::vector<std::wstring> options, int selected) {
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = id;
        row.label = label;
        row.options = std::move(options);
        row.selected = selected;
        rows.push_back(std::move(row));
    };
    const auto slider = [&](SettingId id, int param, const std::wstring& label, float value01,
                            const std::wstring& value, bool enabled = true,
                            bool trailing = false, bool trailingOn = false) {
        PanelRow row;
        row.kind = PanelRowKind::Slider;
        row.id = id;
        row.param = param;
        row.label = label;
        row.slider01 = std::clamp(value01, 0.0F, 1.0F);
        row.value = value;
        row.enabled = enabled;
        row.trailingToggle = trailing;
        row.trailingOn = trailingOn;
        rows.push_back(std::move(row));
    };
    const auto buttons = [&](SettingId id, int param, const std::wstring& label,
                             std::vector<std::wstring> captions, const std::wstring& value = L"",
                             bool enabled = true) {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = id;
        row.param = param;
        row.label = label;
        row.options = std::move(captions);
        row.value = value;
        row.enabled = enabled;
        rows.push_back(std::move(row));
    };
    const auto positionName = [](std::size_t position) {
        return L"V" + std::to_wstring(position + 1);
    };

    if (embyBrowserOpen_) {
        return browserSource_ == BrowserSource::Local ? buildLocalRows(clientWidth) : buildEmbyRows(clientWidth);
    }

    switch (settingsTab_) {
    case SettingsTab::Playback: {
        choice(SettingId::SeekMode, L"Seek bars", {L"Linked", L"Independent"},
               static_cast<int>(perPaneTimelines() ? browserSeekMode_ : seekMode_));
        if (perPaneTimelines()) {
            note(L"Linked: click a seek bar to align videos to the same time; their queues and end rules stay separate.");
            rows.back().wrapNote = true;
            note(L"Independent: use each pane's seek bar; the bottom group timeline is hidden for multiple panes.");
            rows.back().wrapNote = true;
            note(L"Browser seek choice starts Independent and is not saved. The manual-deck choice stays unchanged.");
            rows.back().wrapNote = true;
        }
        choice(SettingId::Decoder, L"Decoder", {L"Automatic", L"Hardware", L"Software"},
               static_cast<int>(decodeMode_));
        note(L"Changing the decoder reopens every video at the same time.");
        toggle(SettingId::RepeatAll, -1, L"Restart all when the set finishes", repeatAll_);
        rows.back().enabled = !perPaneTimelines();
        toggle(SettingId::KeyframeSeek, -1, L"One video seeks to keyframes  (instant; lands up to a keyframe interval off)",
               keyframeSeek_);
        choice(SettingId::PlayOrder, L"When each browser video (or the only other video) ends",
               {L"Stop", L"Repeat it", L"Next, then stop", L"Repeat the list", L"Shuffle"},
               static_cast<int>(playOrder_));

        header(L"Timing");
        note(L"Offset: seconds a video sits ahead of the master timeline; negative keeps it waiting.");
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            const bool loaded = sources_[pane] != nullptr;
            const double offset = effectiveTimelineOffset(
                startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]);
            buttons(SettingId::PaneOffset, static_cast<int>(position),
                    positionName(position) + L" offset", {L"−1 s", L"−0.1 s", L"+0.1 s", L"+1 s"},
                    seconds(offset), loaded);
        }
        buttons(SettingId::ResetOffsets, -1, L"", {L"Reset all offsets to zero"}, L"", anyPaneOffset());
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            toggle(SettingId::PaneRepeat, static_cast<int>(position),
                   positionName(position) + L" repeats", sourceAutoRepeat_[pane],
                   activeSeekMode() == SeekMode::Independent && sources_[pane] != nullptr);
        }
        note(perPaneTimelines()
            ? L"Per-video repeat remains available with Linked browser seeks."
            : L"Per-video repeat needs Independent seek bars and several videos.");
        break;
    }

    case SettingsTab::Audio: {
        slider(SettingId::MasterVolume, -1, L"Master volume", audio_.volume(), percent(audio_.volume()));
        toggle(SettingId::Mute, -1, L"Mute", audio_.muted());
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            const bool loaded = sources_[pane] != nullptr;
            toggle(SettingId::PaneAudio, static_cast<int>(position),
                   positionName(position) + L" audio output", audioPaneEnabled(pane), loaded);
        }
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            const bool loaded = sources_[pane] != nullptr;
            slider(SettingId::PaneVolume, static_cast<int>(position),
                   positionName(position) + L" volume", audio_.paneVolume(pane),
                   percent(audio_.paneVolume(pane)), loaded, true, audio_.paneMuted(pane));
        }
        break;
    }

    case SettingsTab::Subtitles: {
        toggle(SettingId::SubtitleShow, -1, L"Show subtitles  (Alt+H)", subtitleSettings_.show);
        {
            std::vector<std::wstring> languages;
            for (int language = 0; language < kSubtitleLanguageCount; ++language) {
                languages.emplace_back(subtitleLanguageName(clampSubtitleLanguage(language)));
            }
            choice(SettingId::SubtitleLanguage, L"Language, where a video offers several",
                   std::move(languages), subtitleSettings_.language);
        }
        note(L"Auto: the languages Windows lists, those other than English first.");
        slider(SettingId::SubtitleSize, -1, L"Size  (Alt+PgUp / PgDn)",
               (subtitleSettings_.size - kSubtitleSizeMinimum) / (kSubtitleSizeMaximum - kSubtitleSizeMinimum),
               std::to_wstring(static_cast<int>(std::lround(subtitleSettings_.size * 100.0F))) + L"%");
        slider(SettingId::SubtitlePosition, -1, L"Raised above their own place  (Alt+Up / Down)",
               subtitleSettings_.position / kSubtitlePositionMaximum, percent(subtitleSettings_.position));
        toggle(SettingId::SubtitleBackground, -1, L"Dark box behind plain (SRT) subtitles", subtitleSettings_.background);
        note(L"ASS subtitles are drawn as their script says: its fonts, colours and places, and the fonts the "
             L"video brought. Size and raising apply to their speech, not to the signs the script places.");
        rows.back().wrapNote = true;
        bool subtitleRows = false;
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            if (!paneLogicallyLoaded(pane)) continue;
            subtitleRows = true;
            // What the video shows, its timing, and the ways to change both.
            std::wstring shown = subtitleLabel(pane);
            if (shown.size() > 34) shown = shown.substr(0, 33) + L"\x2026";
            buttons(SettingId::SubtitlePane, static_cast<int>(position),
                    positionName(position) + L"  \x00B7  " + shown,
                    {L"Next", L"Earlier", L"Later", L"Load\x2026"}, seconds(subtitleDelay_[pane]));
        }
        note(subtitleRows ? L"Next: Alt+L.  Earlier / later by 0.5 s:  .  and  ,  (/ resets).  Load: Alt+O."
                          : L"Open a video to choose its subtitles, or drop a subtitle file on it.");
        break;
    }

    case SettingsTab::Picture: {
        header(L"Layout");
        {
            std::vector<std::wstring> arrangements;
            for (int mode = 0; mode < 6; ++mode) arrangements.emplace_back(layoutModeName(static_cast<LayoutMode>(mode)));
            choice(SettingId::Layout, L"Arrangement  (also on the bar and in the menu with several videos)",
                   std::move(arrangements), static_cast<int>(layoutMode_));
        }
        const bool commonViewMode = std::all_of(
            paneViews_.begin(), paneViews_.end(),
            [&](const PaneView& view) { return view.mode == paneViews_[0].mode; });
        choice(SettingId::AllView, L"All videos", {L"Fit", L"Fill / crop", L"Stretch"},
               commonViewMode ? static_cast<int>(paneViews_[0].mode) : -1);
        std::wostringstream zoom;
        zoom << std::fixed << std::setprecision(2) << paneViews_[0].zoom << L"×";
        buttons(SettingId::ZoomAll, -1, L"Zoom all", {L"−0.25×", L"1×", L"+0.25×"}, zoom.str());

        header(L"Shader");
        std::vector<std::wstring> shaders{L"Normal", L"Sharpen", L"Grayscale", L"Invert", L"Vibrance+"};
        int selectedShader = customShaderPath_.empty() ? static_cast<int>(shaderPreset_) : 5;
        if (!customShaderPath_.empty()) {
            shaders.push_back(std::filesystem::path(customShaderPath_).filename().wstring());
        }
        choice(SettingId::Shader, L"Pixel shader", std::move(shaders), selectedShader);
        buttons(SettingId::ShaderFile, -1, L"", {L"Load .txt / .hlsl…", L"Disable shader"});
        if (!customShaderPath_.empty()) note(customShaderPath_);
        const auto vibrance = clampSmartVibranceSettings(smartVibrance_);
        const std::array<float, 4> vibranceValues{
            vibrance.intensity, vibrance.saturationPivot, vibrance.grayPivot, vibrance.graySharpness};
        constexpr std::array<const wchar_t*, 4> vibranceLabels{
            L"Vibrance intensity", L"Saturation pivot", L"Gray pivot", L"Gray sharpness"};
        constexpr std::array<int, 4> vibrancePrecision{2, 2, 4, 0};
        for (std::size_t index = 0; index < 4; ++index) {
            std::wostringstream value;
            value << std::fixed << std::setprecision(vibrancePrecision[index]) << vibranceValues[index];
            const float fraction = (vibranceValues[index] - kVibranceMin[index]) /
                                   (kVibranceMax[index] - kVibranceMin[index]);
            slider(SettingId::Vibrance, static_cast<int>(index), vibranceLabels[index], fraction, value.str());
        }
        note(L"The four Vibrance+ values are saved with settings and visual presets.");
        note(L"Vibrance+: RTX HDR enhanced; native HDR unchanged. 1.00 = neutral.");
        toggle(SettingId::SuperResolution, -1, L"NVIDIA RTX Video Super Resolution", rtxVideo_.superResolution);
        toggle(SettingId::RtxHdr, -1, L"NVIDIA RTX Video HDR  (SDR video shown as HDR)", rtxVideo_.rtxHdr);
        note(videoEnhancementNote(rtxVideo_, renderer_.enhancementStatus()));
        break;
    }

    case SettingsTab::General: {
        toggle(SettingId::PinBar, -1, L"Keep the control bar visible  (U)", controlsPinned_);
        toggle(SettingId::NasCache, -1, L"NAS read-ahead cache for new videos", nasCacheEnabled_);
        {
            std::wostringstream status;
            std::size_t total = 0;
            bool any = false;
            for (std::size_t position = 0; position < kMaxPanes; ++position) {
                const auto pane = paneForPosition(position);
                if (!sources_[pane]) continue;
                const auto cache = sources_[pane]->cacheStats();
                if (!cache.enabled) continue;
                total += cache.residentBytes;
                if (any) status << L"   ";
                any = true;
                status << positionName(position) << L" " << static_cast<int>(cache.aheadSeconds) << L"s";
            }
            if (any) status << L"   (" << total / (1024 * 1024) << L" MiB)";
            else status << L"No video is reading through the cache";
            note(status.str());
        }
        buttons(SettingId::FileTypes, -1, L"Open videos with QuadDeck from Explorer",
                {L"Register\x2026", L"Remove"});
        note(associationNote(fileTypes_));
        buttons(SettingId::Style, -1, L"Visual preset  (layout, view, shader; no videos)",
                {L"Load…", L"Save…"});
        buttons(SettingId::Session, -1, L"Session  (videos, positions, everything)",
                {L"Open…  Ctrl+O", L"Save…  Ctrl+S"});

        break;
    }
    }
    return rows;
}

void App::refreshPanelGeometry(const RECT& client) {
    const float clientWidth = static_cast<float>(client.right);
    // The browser is the whole window with nothing playing and docked at
    // the right beside a video; normal settings keep their sheet.
    float sheetWidth = 0.0F;
    if (embyBrowserOpen_) sheetWidth = embyBrowserDocked() ? embyDockWidth(clientWidth) : clientWidth;
    panelRows_ = buildSettingsRows(sheetWidth > 0.0F ? sheetWidth : std::min(clientWidth, PanelMetrics{}.width * uiScale_));
    const float rowWidth = std::max(0.0F, (sheetWidth > 0.0F ? sheetWidth :
        std::min(clientWidth, PanelMetrics{}.width * uiScale_)) - 2.0F * PanelMetrics{}.pad * uiScale_);
    for (auto& row : panelRows_) {
        if (row.kind == PanelRowKind::MediaDetail) overlay_.measureMediaDetail(row, rowWidth, uiScale_);
        if (row.kind == PanelRowKind::Note && row.wrapNote) overlay_.measureWrappedNote(row, rowWidth, uiScale_);
    }
    panelLayout_ = settingsPanelLayout(
        clientWidth, static_cast<float>(client.bottom), uiScale_,
        panelRows_, settingsScroll_, settingsAlpha_, PanelMetrics{}, sheetWidth, captionInset(),
        embyBrowserOpen_ ? 0 : kSettingsTabCount, true);
    settingsScroll_ = std::clamp(settingsScroll_, 0.0F, panelLayout_.maxScroll);
    if (embyBrowserOpen_ && settingsOpen_) embyRequestVisibleImages();
    if (embyBrowserOpen_ && settingsOpen_ && browserSource_ == BrowserSource::Emby) embyLoadMoreNearEnd();
}

PanelHit App::panelHitAt(POINT local) const {
    if (settingsAlpha_ <= 0.05F) return {};
    return settingsPanelHitTest(static_cast<float>(local.x), static_cast<float>(local.y),
                                panelLayout_, panelRows_, kPanelRailInset * uiScale_);
}

void App::scrollSettingsPanel(int wheelDelta) {
    settingsScroll_ = std::clamp(
        settingsScroll_ - static_cast<float>(wheelDelta) / WHEEL_DELTA * 56.0F * uiScale_,
        0.0F, panelLayout_.maxScroll);
}

void App::applyPanelSlider(int rowIndex, float fraction) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(panelRows_.size())) return;
    const auto& row = panelRows_[static_cast<std::size_t>(rowIndex)];
    fraction = std::clamp(fraction, 0.0F, 1.0F);
    switch (row.id) {
    case SettingId::MasterVolume:
        audio_.setVolume(fraction);
        if (fraction > 0.0F && audio_.muted()) audio_.setMuted(false);
        break;
    case SettingId::PaneVolume:
        if (row.param >= 0) audio_.setPaneVolume(paneForPosition(static_cast<std::size_t>(row.param)), fraction);
        break;
    case SettingId::Vibrance:
        if (row.param >= 0 && row.param < 4) {
            const auto index = static_cast<std::size_t>(row.param);
            const float value = kVibranceMin[index] + (kVibranceMax[index] - kVibranceMin[index]) * fraction;
            switch (index) {
            case 0: smartVibrance_.intensity = value; break;
            case 1: smartVibrance_.saturationPivot = value; break;
            case 2: smartVibrance_.grayPivot = value; break;
            default: smartVibrance_.graySharpness = value; break;
            }
            smartVibrance_ = clampSmartVibranceSettings(smartVibrance_);
            renderer_.setSmartVibranceSettings(smartVibrance_);
        }
        break;
    case SettingId::SubtitleSize:
        // In steps of five per cent, so the same size can be found again.
        subtitleSettings_.size = std::round(
            (kSubtitleSizeMinimum + (kSubtitleSizeMaximum - kSubtitleSizeMinimum) * fraction) * 20.0F) / 20.0F;
        subtitleSettings_ = clampSubtitleSettings(subtitleSettings_);
        break;
    case SettingId::SubtitlePosition:
        subtitleSettings_.position = std::round(kSubtitlePositionMaximum * fraction * 100.0F) / 100.0F;
        subtitleSettings_ = clampSubtitleSettings(subtitleSettings_);
        break;
    default:
        break;
    }
}

void App::activatePanelHit(const PanelHit& hit) {
    switch (hit.kind) {
    case PanelHitKind::Close:
    case PanelHitKind::Outside:
        closeSettingsPanel();
        return;
    case PanelHitKind::Tab:
        if (hit.part >= 0 && hit.part < kSettingsTabCount) selectSettingsTab(static_cast<SettingsTab>(hit.part));
        return;
    case PanelHitKind::Switch:
        switchSheet();
        return;
    case PanelHitKind::Toggle:
    case PanelHitKind::Segment:
    case PanelHitKind::Button:
    case PanelHitKind::Trailing:
    case PanelHitKind::Item:
    case PanelHitKind::Tile:
    case PanelHitKind::Add:
        break;
    default:
        return;
    }
    if (hit.row < 0 || hit.row >= static_cast<int>(panelRows_.size())) return;
    const PanelRow row = panelRows_[static_cast<std::size_t>(hit.row)];
    const std::size_t position = row.param >= 0 ? static_cast<std::size_t>(row.param) : 0;
    const std::size_t pane = row.param >= 0 && position < kMaxPanes ? paneForPosition(position) : 0;
    switch (row.id) {
    case SettingId::RepeatAll:
        repeatAll_ = !repeatAll_;
        showNotice(repeatAll_ ? L"Restart all when finished: on" : L"Restart all when finished: off");
        break;
    case SettingId::SeekMode:
        if (hit.part >= 0 && hit.part <= 1) chooseSeekMode(static_cast<SeekMode>(hit.part));
        break;
    case SettingId::Decoder:
        if (hit.part >= 0 && hit.part <= 2) {
            changeDecodeMode(static_cast<DecodeMode>(hit.part));
            constexpr std::array<const wchar_t*, 3> names{L"Automatic", L"Hardware", L"Software"};
            showNotice(std::wstring(L"Decoder: ") + names[static_cast<std::size_t>(hit.part)]);
        }
        break;
    case SettingId::Mute:
        toggleMute();
        break;
    case SettingId::PaneAudio:
        toggleAudioPane(pane);
        break;
    case SettingId::PaneVolume:
        if (hit.kind == PanelHitKind::Trailing) togglePaneMute(pane);
        break;
    case SettingId::PaneOffset: {
        constexpr std::array<double, 4> steps{-1.0, -0.1, 0.1, 1.0};
        if (hit.part >= 0 && hit.part < 4 && sources_[pane]) {
            const double current = effectiveTimelineOffset(
                startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]);
            setPaneOffset(pane, current + steps[static_cast<std::size_t>(hit.part)]);
        }
        break;
    }
    case SettingId::ResetOffsets:
        resetAllOffsets();
        break;
    case SettingId::PaneRepeat:
        setSourceAutoRepeat(pane, !sourceAutoRepeat_[pane]);
        break;
    case SettingId::Layout:
        if (hit.part >= 0 && hit.part <= 5) chooseLayout(static_cast<LayoutMode>(hit.part));
        break;
    case SettingId::AllView:
        if (hit.part >= 0 && hit.part <= 2) {
            for (auto& view : paneViews_) view.mode = static_cast<ViewMode>(hit.part);
            showNotice(L"All videos  " + std::wstring(viewModeName(static_cast<ViewMode>(hit.part))));
        }
        break;
    case SettingId::ZoomAll:
        for (auto& view : paneViews_) {
            if (hit.part == 0) view.zoom = std::max(1.0F, view.zoom - 0.25F);
            else if (hit.part == 2) view.zoom = std::min(4.0F, view.zoom + 0.25F);
            else view.zoom = 1.0F;
        }
        break;
    case SettingId::Shader:
        if (hit.part >= 0 && hit.part <= 4) {
            if (applyShaderPreset(static_cast<ShaderPreset>(hit.part))) {
                showNotice(L"Shader: " + row.options[static_cast<std::size_t>(hit.part)]);
            }
        } else if (hit.part == 5 && !customShaderPath_.empty() && !deviceRecoveryPending_ &&
                   !renderer_.deviceLost() && !renderer_.loadPixelShader(customShaderPath_)) {
            MessageBoxA(window_, renderer_.error().c_str(), "Pixel shader error", MB_ICONERROR);
        }
        break;
    case SettingId::ShaderFile:
        if (hit.part == 0) loadShaderDialog();
        else applyShaderPreset(ShaderPreset::Normal);
        break;
    case SettingId::PinBar:
        toggleControls();
        break;
    case SettingId::NasCache:
        nasCacheEnabled_ = !nasCacheEnabled_;
        showNotice(nasCacheEnabled_ ? L"NAS cache on for new videos" : L"NAS cache off for new videos");
        break;
    case SettingId::SuperResolution:
        rtxVideo_.superResolution = !rtxVideo_.superResolution;
        renderer_.setVideoEnhancements(rtxVideo_);
        showNotice(rtxVideo_.superResolution ? L"RTX Super Resolution on" : L"RTX Super Resolution off");
        break;
    case SettingId::RtxHdr:
        rtxVideo_.rtxHdr = !rtxVideo_.rtxHdr;
        renderer_.setVideoEnhancements(rtxVideo_);
        showNotice(rtxVideo_.rtxHdr ? L"RTX Video HDR on" : L"RTX Video HDR off");
        break;
    case SettingId::Style:
        if (hit.part == 0) loadStyleDialog();
        else saveStyleDialog();
        break;
    case SettingId::Session:
        if (hit.part == 0) loadSessionDialog();
        else saveSessionDialog();
        break;
    case SettingId::EmbyAccount:
        embyAccountAction(hit.part);
        break;
    case SettingId::EmbyNav:
        embyNavigate(hit.part);
        break;
    case SettingId::EmbyItem:
        if (hit.kind == PanelHitKind::Add) embyAddItem(panelRowParam(row, hit.part));
        else embyActivateItem(panelRowParam(row, hit.part));
        break;
    case SettingId::EmbyReplaceChoice:
        embyConfirmReplacement(static_cast<std::size_t>(panelRowParam(row, hit.part)));
        break;
    case SettingId::EmbyReplaceCancel:
        embyCancelReplacement();
        break;
    case SettingId::EmbyLibraryDetails:
        if (row.param >= 0 && static_cast<std::size_t>(row.param) < embyViews_.size()) {
            embyToggleLibraryDetails(embyViews_[static_cast<std::size_t>(row.param)].id);
        } else if (row.param < 0) {
            embyToggleLibraryDetails();
        }
        break;
    case SettingId::EmbyDetailAction:
        embyDetailAction(hit.part);
        break;
    case SettingId::EmbyDetailSeason:
        embyChooseDetailSeason(hit.part);
        break;
    case SettingId::EmbyDetailEpisode:
        embyDetailEpisodeAction(row.param, hit.part);
        break;
    case SettingId::EmbyMore:
        embyLoadMore();
        break;
    case SettingId::EmbyView:
        embySetView(hit.part);
        break;
    case SettingId::EmbySort:
        embyChooseSort(hit.part);
        break;
    case SettingId::EmbyUnplayed:
        embyToggleUnplayed();
        break;
    case SettingId::EmbyFlat:
        embySetFlat(hit.part == 1);
        break;
    case SettingId::PlayOrder:
        if (hit.part >= 0 && hit.part < kPlayOrderCount) setPlayOrder(clampPlayOrder(hit.part));
        break;
    case SettingId::KeyframeSeek:
        keyframeSeek_ = !keyframeSeek_;
        showNotice(keyframeSeek_ ? L"One video seeks to keyframes" : L"One video seeks exactly");
        break;
    case SettingId::LocalNav:
        localNavigate(hit.part);
        break;
    case SettingId::LocalSort:
        if (hit.part >= 0 && hit.part < kLocalSortCount) sortListBy(static_cast<int>(localSortKey(hit.part)));
        break;
    case SettingId::LocalItem:
        activateLocalItem(panelRowParam(row, hit.part), hit.kind == PanelHitKind::Add);
        break;
    case SettingId::LocalReplaceChoice:
        localConfirmReplacement(static_cast<std::size_t>(panelRowParam(row, hit.part)));
        break;
    case SettingId::LocalReplaceCancel:
        localCancelReplacement();
        break;
    case SettingId::FileTypes:
        fileTypesAction(hit.part);
        break;
    case SettingId::SubtitleShow:
        toggleSubtitlesShown();
        break;
    case SettingId::SubtitleLanguage:
        if (hit.part >= 0 && hit.part < kSubtitleLanguageCount) setSubtitleLanguage(hit.part);
        break;
    case SettingId::SubtitleBackground:
        subtitleSettings_.background = !subtitleSettings_.background;
        scheduleAppSettingsSave();
        break;
    case SettingId::SubtitlePane:
        if (paneLogicallyLoaded(pane)) {
            if (hit.part == 0) cycleSubtitle(pane);
            else if (hit.part == 1) nudgeSubtitleDelay(pane, -0.5);
            else if (hit.part == 2) nudgeSubtitleDelay(pane, 0.5);
            else if (hit.part == 3) loadSubtitleDialog(pane);
        }
        break;
    default:
        break;
    }
    updateTitle();
    scheduleAppSettingsSave();
}

void App::releasePanelPress(POINT local) {
    const PanelHitKind kind = pressedPanelKind_;
    const int row = pressedPanelRow_;
    const int part = pressedPanelPart_;
    const SettingId id = pressedPanelId_;
    const int param = pressedPanelParam_;
    pressedPanelKind_ = PanelHitKind::None;
    pressedPanelRow_ = pressedPanelPart_ = -1;
    pressedPanelId_ = SettingId::None;
    pressedPanelParam_ = -1;
    if (GetCapture() == videoWindow_) ReleaseCapture();
    const PanelHit panelHit = panelHitAt(local);
    if (panelHit.kind != kind || panelHit.row != row || panelHit.part != part) return;
    // The rows are rebuilt every frame and a reply from the server can
    // insert some between press and release; the same row number must
    // still be the same row.
    if (row >= 0 && row < static_cast<int>(panelRows_.size())) {
        const auto& current = panelRows_[static_cast<std::size_t>(row)];
        if (current.id != id || panelRowParam(current, part) != param) return;
    }
    activatePanelHit(panelHit);
}

}  // namespace quaddeck
