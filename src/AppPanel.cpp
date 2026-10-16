// App: the settings sheet. Fills the SettingsPanel rows from player state,
// turns hits on them into the same state changes the popup menu and keys
// make, and owns the sheet's open/close, scroll and slide.

#include "App.hpp"
#include "AppInternal.hpp"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace quaddeck {

using namespace app_internal;

namespace {

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
    // F5 and Settings in the menu always address normal settings, even
    // when the shared sheet last showed Emby or still shows it now.
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
    return embyBrowserOpen_ && anyPaneLoaded() && !embySignInPage();
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
    if (!embyBrowserOpen_) {
        return settingsTabScroll_[static_cast<std::size_t>(settingsTab_)];
    }
    if (browserSource_ == BrowserSource::Emby && embyDetails_) return embyDetails_->scroll;
    return browserSource_ == BrowserSource::Local ? localListScroll_ : embyListScroll_;
}

void App::rememberSheetScroll() { sheetScrollSlot() = settingsScroll_; }

void App::restoreSheetScroll() { settingsScroll_ = sheetScrollSlot(); }

// The header switches between settings and the Emby browser.
std::vector<SheetFace> App::sheetSwitches() const {
    return {embyBrowserOpen_ ? SheetFace::Settings : SheetFace::Emby};
}

void App::switchSheet(int part) {
    if (!settingsOpen_) return;
    const auto switches = sheetSwitches();
    if (part < 0 || static_cast<std::size_t>(part) >= switches.size()) return;
    showSheetFace(switches[static_cast<std::size_t>(part)]);
}

// Signed out, the Emby side is the sign-in page, as Ctrl+E. Each face
// takes the sheet at once, keeping its own scroll.
void App::showSheetFace(SheetFace face) {
    controlsLastInteraction_ = GetTickCount64();
    if (face == SheetFace::Emby) {
        embyOpenBrowser();
        return;
    }
    if (!settingsOpen_) toggleSettingsPanel();
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

std::vector<PanelRow> App::buildSettingsRows(float clientWidth, std::vector<PanelListRun>* runs) const {
    if (runs) runs->clear();
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
        row.wrapNote = true;
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
        return browserSource_ == BrowserSource::Local ? buildLocalRows(clientWidth, runs)
                                                      : buildEmbyRows(clientWidth, runs);
    }

    switch (settingsTab_) {
    case SettingsTab::Playback: {
        choice(SettingId::SeekMode, L"Seek bars", {L"Linked", L"Independent"},
               static_cast<int>(perPaneTimelines() ? browserSeekMode_ : seekMode_));
        rows.back().hint = perPaneTimelines()
            ? L"Linked aligns source times. Independent uses each video's lower edge. Queues remain separate."
            : L"Linked moves the shared timeline. Independent uses each video's lower edge.";
        choice(SettingId::Decoder, L"Decoder", {L"Automatic", L"Hardware", L"Software"},
               static_cast<int>(decodeMode_));
        note(L"Changing the decoder reopens every video at the same time.");
        toggle(SettingId::RepeatAll, -1, L"Restart all when the set finishes", repeatAll_);
        rows.back().enabled = !perPaneTimelines();
        toggle(SettingId::KeyframeSeek, -1, L"Fast seeking for one video",
               keyframeSeek_);
        rows.back().hint = L"Land on a nearby keyframe for faster seeking. Turn off for exact positioning.";
        choice(SettingId::PlayOrder, L"When a video ends",
               {L"Stop", L"Repeat it", L"Next, then stop", L"Repeat the list", L"Shuffle"},
               static_cast<int>(playOrder_));

        header(L"Timing");
        note(L"Offset: seconds a video sits ahead of the master timeline; negative keeps it waiting.");
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            const bool loaded = sources_[pane] != nullptr;
            if (!loaded) continue;
            const double offset = effectiveTimelineOffset(
                startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]);
            buttons(SettingId::PaneOffset, static_cast<int>(position),
                    positionName(position) + L" offset", {L"−1 s", L"−0.1 s", L"+0.1 s", L"+1 s"},
                    seconds(offset), loaded);
        }
        buttons(SettingId::ResetOffsets, -1, L"", {L"Reset all offsets to zero"}, L"", anyPaneOffset());
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            if (!sources_[pane] || singleLoadedPane() >= 0) continue;
            toggle(SettingId::PaneRepeat, static_cast<int>(position),
                   positionName(position) + L" repeats", sourceAutoRepeat_[pane],
                   activeSeekMode() == SeekMode::Independent && sources_[pane] != nullptr &&
                       paneRepeatFlagInForce(paneOrigins(), pane, seekMode_));
        }
        note(perPaneTimelines()
            ? L"F6 and Emby videos repeat on their own; a video opened by hand repeats only among "
              L"several opened by hand, with Independent seek bars."
            : L"Per-video repeat needs Independent seek bars and several videos.");
        break;
    }

    case SettingsTab::Audio: {
        slider(SettingId::MasterVolume, -1, L"Master volume", audio_.volume(), percent(audio_.volume()));
        toggle(SettingId::Mute, -1, L"Mute", audio_.muted());
        for (std::size_t position = 0; position < kMaxPanes; ++position) {
            const auto pane = paneForPosition(position);
            const bool loaded = sources_[pane] != nullptr;
            if (!loaded) continue;
            toggle(SettingId::PaneAudio, static_cast<int>(position),
                   positionName(position) + L" audio output", audioPaneEnabled(pane), loaded);
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
            choice(SettingId::SubtitleLanguage, L"Preferred language",
                   std::move(languages), subtitleSettings_.language);
        }
        note(L"Auto: the languages Windows lists, those other than English first.");
        slider(SettingId::SubtitleSize, -1, L"Size  (Alt+PgUp / PgDn)",
               (subtitleSettings_.size - kSubtitleSizeMinimum) / (kSubtitleSizeMaximum - kSubtitleSizeMinimum),
               std::to_wstring(static_cast<int>(std::lround(subtitleSettings_.size * 100.0F))) + L"%");
        slider(SettingId::SubtitlePosition, -1, L"Raise subtitles  (Alt+Up / Down)",
               subtitleSettings_.position / kSubtitlePositionMaximum, percent(subtitleSettings_.position));
        toggle(SettingId::SubtitleBackground, -1, L"Subtitle background", subtitleSettings_.background);
        rows.back().hint = L"Dark background for plain text subtitles. Styled ASS subtitles keep their script's design.";
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
            choice(SettingId::Layout, L"Arrangement",
                   std::move(arrangements), static_cast<int>(layoutMode_));
        }
        std::size_t reference = 0;
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            if (paneLogicallyLoaded(pane)) { reference = pane; break; }
        }
        bool commonViewMode = true, commonZoom = true;
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            if (!paneLogicallyLoaded(pane)) continue;
            commonViewMode = commonViewMode && paneViews_[pane].mode == paneViews_[reference].mode;
            commonZoom = commonZoom && std::abs(paneViews_[pane].zoom - paneViews_[reference].zoom) < 0.001F;
        }
        choice(SettingId::AllView, L"All videos", {L"Fit", L"Fill / crop", L"Stretch"},
               commonViewMode ? static_cast<int>(paneViews_[reference].mode) : -1);
        std::wostringstream zoom;
        if (commonZoom) zoom << std::fixed << std::setprecision(2) << paneViews_[reference].zoom << L"×";
        else zoom << L"Mixed";
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
        if (customShaderUnloaded_) {
            note(L"Remembered but not loaded: the file could not be read, so the preset runs. "
                 L"Choose it above to try again; the next start tries it too.");
        }
        // Only the Vibrance+ shader reads these four values, so they are
        // listed only while it is the one running.
        if (selectedShader == static_cast<int>(ShaderPreset::SmartVibrancePlus)) {
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
        }
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

App::ListRowMarks App::listRowMarks() const {
    ListRowMarks marks;
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        if (embyPaneAccountMatches(pane) && !embyPanes_[pane]->itemId.empty()) {
            marks.embyPlaying.insert(embyPanes_[pane]->itemId);
        }
    }
    marks.localPlaying.fill(-1);
    if (browserSource_ == BrowserSource::Local) {
        for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
            const auto& path = paths_[pane];
            if (!emby::isLocator(path) && sameLocalDirectory(localDirectory(path), localDirectory_)) {
                marks.localPlaying[pane] = localEntryIndex(localList_, path);
            }
        }
    }
    // All libraries must finish before an item's provenance or final
    // position is known. Opening a partial result would retire the
    // remaining replies and leave Back with an incomplete search.
    marks.embyItemsEnabled =
        !(embyLoading_ && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search);
    return marks;
}

void App::fillListRow(PanelRow& row, const ListRowMarks& marks) const {
    if (row.id == SettingId::LocalItem) fillLocalRowContent(row, marks);
    else fillEmbyRowContent(row, marks);
}

// The builders' rows with their lists' runs: handed back for
// refreshPanelGeometry, or put in and filled, every row whole.
std::vector<PanelRow> App::withListRuns(std::vector<PanelRow> rows, std::vector<PanelListRun> listRuns,
                                        std::vector<PanelListRun>* runs) const {
    if (runs) {
        *runs = std::move(listRuns);
        return rows;
    }
    if (listRuns.empty()) return rows;
    auto all = panelRowsWithRuns(std::move(rows), listRuns);
    const auto marks = listRowMarks();
    for (const auto& run : listRuns) {
        for (std::size_t index = run.at; index < run.at + run.rowCount() && index < all.size(); ++index) {
            fillListRow(all[index], marks);
        }
    }
    return all;
}

// This frame's rows into panelRows_. While the lists keep their shape --
// the same runs in the same places -- their rows stay as they are and only
// the others are replaced, so a long list costs a frame nothing for the rows
// it does not show. Those filled last frame go back to bare here; the ones
// near the screen are filled again once laid out (fillListWindow), from
// whatever their entries hold now.
void App::composePanelRows(std::vector<PanelRow> rows, std::vector<PanelListRun> runs) {
    std::size_t listed = 0;
    for (const auto& run : runs) listed += run.rowCount();
    const bool keep = !runs.empty() && runs.size() == panelRuns_.size() &&
        rows.size() + listed == panelRows_.size() &&
        std::equal(runs.begin(), runs.end(), panelRuns_.begin(), [](const PanelListRun& left, const PanelListRun& right) {
            return left.at == right.at && left.sameShape(right);
        });
    if (!keep) {
        panelRows_ = panelRowsWithRuns(std::move(rows), runs);
        panelRuns_ = std::move(runs);
        panelFilledRows_.clear();
        return;
    }
    std::size_t next = 0;
    forEachRowOutsideRuns(panelRows_.size(), panelRuns_, [&](std::size_t index) {
        panelRows_[index] = std::move(rows[next++]);
    });
    for (const std::size_t index : panelFilledRows_) {
        for (const auto& run : panelRuns_) {
            if (!run.contains(index)) continue;
            panelRows_[index] = run.bareRow(index - run.at);
            break;
        }
    }
    panelFilledRows_.clear();
}

// The bare list rows the sheet shows, and a screen above and below so a
// scroll finds them filled: an Item or Tiles row's height does not depend on
// its text, so the layout made without it stands and only their parts are
// placed now. The content's rows follow each other down, so the search
// starts at the first one reaching the band and ends below it.
void App::fillListWindow() {
    if (panelRuns_.empty()) return;
    const auto& content = panelLayout_.content;
    const float margin = content.height;
    const float top = content.y - margin;
    const float bottom = content.bottom() + margin;
    const auto marks = listRowMarks();
    const std::size_t count = std::min(panelRows_.size(), panelLayout_.rows.size());
    for (std::size_t i = panelFirstRowReaching(panelLayout_, top); i < count; ++i) {
        auto& geometry = panelLayout_.rows[i];
        if (geometry.row.y > bottom) break;
        const auto run = std::find_if(panelRuns_.begin(), panelRuns_.end(),
                                      [i](const PanelListRun& candidate) { return candidate.contains(i); });
        if (run == panelRuns_.end()) continue;
        auto& row = panelRows_[i];
        // From bare, whatever the row was given since it was composed.
        if (!row.contentPending) row = run->bareRow(i - run->at);
        fillListRow(row, marks);
        panelRowParts(geometry, row, uiScale_);
        panelFilledRows_.push_back(i);
    }
}

void App::refreshPanelGeometry(const RECT& client) {
    const float clientWidth = static_cast<float>(client.right);
    // The browser is the whole window with nothing playing and docked at
    // the right beside a video; normal settings and the sign-in page keep
    // their sheet.
    float sheetWidth = 0.0F;
    if (embyBrowserOpen_ && !embySignInPage()) {
        sheetWidth = embyBrowserDocked() ? embyDockWidth(clientWidth) : clientWidth;
    }
    // Every frame: the rows but a browser list's, which come as runs and
    // whose rows stay while the list keeps its shape.
    std::vector<PanelListRun> runs;
    auto rows = buildSettingsRows(sheetWidth > 0.0F ? sheetWidth : std::min(clientWidth, PanelMetrics{}.width * uiScale_),
                                  &runs);
    const float rowWidth = std::max(0.0F, (sheetWidth > 0.0F ? sheetWidth :
        std::min(clientWidth, PanelMetrics{}.width * uiScale_)) - 2.0F * PanelMetrics{}.pad * uiScale_);
    for (auto& row : rows) {
        if (row.kind == PanelRowKind::MediaDetail) overlay_.measureMediaDetail(row, rowWidth, uiScale_);
        if (row.kind == PanelRowKind::Note && row.wrapNote) overlay_.measureWrappedNote(row, rowWidth, uiScale_);
    }
    composePanelRows(std::move(rows), std::move(runs));
    settingsPanelLayoutInto(
        panelLayout_, clientWidth, static_cast<float>(client.bottom), uiScale_,
        panelRows_, settingsScroll_, settingsAlpha_, PanelMetrics{}, sheetWidth, captionInset(),
        embyBrowserOpen_ ? 0 : kSettingsTabCount, static_cast<int>(sheetSwitches().size()));
    settingsScroll_ = std::clamp(settingsScroll_, 0.0F, panelLayout_.maxScroll);
    fillListWindow();
    if (embyBrowserOpen_ && settingsOpen_) embyRequestVisibleImages();
    if (embyBrowserOpen_ && settingsOpen_ && browserSource_ == BrowserSource::Emby) embyLoadMoreNearEnd();
}

bool App::sheetTakesPointer() const { return settingsOpen_ && settingsAlpha_ > 0.05F; }

PanelHit App::panelHitAt(POINT local) const {
    if (!sheetTakesPointer()) return {};
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

unsigned App::showPanelMenu(HMENU menu, SettingId id, int part, int param) {
    if (!menu) return 0;
    POINT anchor{};
    GetCursorPos(&anchor);
    for (std::size_t i = 0; i < panelRows_.size() && i < panelLayout_.rows.size(); ++i) {
        if (panelRows_[i].id != id || (param >= 0 && panelRows_[i].param != param)) continue;
        const auto& geometry = panelLayout_.rows[i];
        if (part >= 0 && part < static_cast<int>(geometry.parts.size()) && geometry.parts[part].visible()) {
            const auto box = geometry.parts[part];
            anchor = {static_cast<LONG>(std::lround(box.x)), static_cast<LONG>(std::lround(box.bottom()))};
            if (videoWindow_) ClientToScreen(videoWindow_, &anchor);
        }
        break;
    }
    const bool previous = contextMenuOpen_;
    contextMenuOpen_ = true;
    const unsigned selected = window_ ? TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, anchor.x, anchor.y, window_, nullptr) : 0;
    contextMenuOpen_ = previous;
    DestroyMenu(menu);
    return selected;
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
        switchSheet(hit.part);
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
                   !renderer_.deviceLost()) {
            if (renderer_.loadPixelShader(customShaderPath_)) customShaderUnloaded_ = false;
            else MessageBoxA(window_, renderer_.error().c_str(), "Pixel shader error", MB_ICONERROR);
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
    case SettingId::EmbyDetailPerson:
        embyOpenPerson(panelRowParam(row, hit.part));
        break;
    case SettingId::EmbyDetailCast:
        if (embyDetails_) embyDetails_->castExpanded = !embyDetails_->castExpanded;
        break;
    case SettingId::EmbyPersonAction:
        embyPersonAction(hit.part);
        break;
    case SettingId::EmbyMore:
        embyLoadMore();
        break;
    case SettingId::EmbyView:
        embySetView(hit.part);
        break;
    case SettingId::EmbyToolbar:
        embyToolbarAction(hit.part);
        break;
    case SettingId::EmbyRetry:
        embyRefreshShown();
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
    case SettingId::EmbySearch:
        if (hit.kind == PanelHitKind::Trailing) embyClearSearch();
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
    case SettingId::LocalToolbar:
        showLocalToolbarMenu(hit.part);
        break;
    case SettingId::LocalRetry:
        retryLocalList();
        break;
    case SettingId::LocalView:
        localSetView(hit.part);
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
