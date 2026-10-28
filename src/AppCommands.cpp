// App: the popup menu and the commands it and the keyboard dispatch --
// pane, view, shader and decode choices, file dialogs and folder stepping.

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
#include <random>
#include <sstream>
#include <utility>

namespace quaddeck {

using namespace app_internal;

namespace {

// How long a step key pressed before its folder was read still counts.
constexpr ULONGLONG kFolderStepWaitMs = 1500;

}  // namespace

void App::setMasterLoopA(double seconds) {
    masterLoopA_ = std::max(0.0, seconds);
    masterLoopEnabled_ = loopArmed(masterLoopA_, masterLoopB_, duration_);
    const bool toEnd = !validLoop(masterLoopA_, masterLoopB_);
    showNotice(L"Loop A  " + formatTime(masterLoopA_) +
               (masterLoopEnabled_ ? (toEnd ? L"  →  end" : L"  ·  loop on") : L""));
    updateTitle();
}

void App::setMasterLoopB(double seconds) {
    masterLoopB_ = std::max(0.0, seconds);
    masterLoopEnabled_ = loopArmed(masterLoopA_, masterLoopB_, duration_);
    showNotice(L"Loop B  " + formatTime(masterLoopB_) +
               (masterLoopEnabled_ ? L"  ·  loop on" : L""));
    updateTitle();
}

void App::showContextMenu(int pane, WindowMenuAnchor anchor) {
    // What a pane is doing is a switch, as the sheet shows it; the rest are
    // commands, and the choices of several a submenu each.
    PanelMenu menu;
    if (pane >= 0 && pane < static_cast<int>(kMaxPanes)) {
        const std::size_t index = static_cast<std::size_t>(pane);
        const bool loaded = paneLogicallyLoaded(index);
        menu.option(CmdOpenPane, loaded ? L"Replace video..." : L"Open video...");
        menu.option(CmdOpenEmby, L"Open from Emby...", false, shortcut(KeyAction::Emby));
        menu.option(CmdPreviousFile, L"Previous file in folder", false, shortcut(KeyAction::PreviousFile), loaded);
        menu.option(CmdNextFile, L"Next file in folder", false, shortcut(KeyAction::NextFile), loaded);
        menu.option(CmdClosePane, L"Close video", false, {}, loaded);
        appendEmbyFavoriteMenu(menu, index);
        menu.check(CmdSelectAudio, L"Output this audio", audioPaneEnabled(index),
                   shortcut(audioKeyAction(positionForPane(index))), loaded);
        menu.check(CmdPaneMute, L"Mute this video", audio_.paneMuted(index), {}, loaded);
        menu.check(CmdTogglePanePause, L"Pause this video", sources_[index] && sourcePaused_[index], {},
                   sources_[index] != nullptr);
        const bool onlyVideo = singleLoadedPane() == pane;
        menu.check(CmdTogglePaneRepeat,
                   onlyVideo ? L"Repeat this video"
                       : !paneRepeatFlagInForce(paneOrigins(), index, seekMode_)
                       ? L"Repeat this video (not beside F6 or Emby videos)"
                       : linkedBrowserSeekBars() ? L"Repeat this video (with the others, Linked)"
                       : L"Repeat this video (Independent mode)",
                   onlyVideo ? playOrder_ == PlayOrder::RepeatOne : sourceAutoRepeat_[index], {}, loaded);
        if (severalPanesLoaded()) {
            menu.check(CmdToggleSolo, L"Solo this video", soloPane_ == pane,
                       shortcut(soloKeyAction(positionForPane(index))), loaded);
        }
        menu.option(CmdResetPaneOffset, L"Reset this video's offset", false, {},
                    sources_[index] && paneOffsetSeconds(index) != 0.0);
        const auto active = activePanes();
        const auto activeCount = static_cast<std::size_t>(
            std::count(active.begin(), active.end(), true));
        if (supportsExpandedPaneCount(activeCount)) {
            // On, the arrangement gives this video the focus; off, it chooses.
            menu.check(CmdToggleExpanded, L"Make this video the focus", expandedPane_ == pane);
        }
        menu.separator();

        PanelMenu speed;
        const auto speedItem = [&](unsigned id, double value, const wchar_t* text) {
            speed.option(id, text, std::abs(playbackRates_[index] - value) < 0.001);
        };
        speedItem(CmdSpeed025, 0.25, L"0.25x"); speedItem(CmdSpeed050, 0.5, L"0.5x");
        speedItem(CmdSpeed100, 1.0, L"1.0x"); speedItem(CmdSpeed150, 1.5, L"1.5x");
        speedItem(CmdSpeed200, 2.0, L"2.0x");
        menu.submenu(L"Playback speed", std::move(speed));

        PanelMenu view;
        view.option(CmdViewFit, L"Fit", paneViews_[index].mode == ViewMode::Fit)
            .option(CmdViewFill, L"Fill", paneViews_[index].mode == ViewMode::Fill)
            .option(CmdViewStretch, L"Stretch", paneViews_[index].mode == ViewMode::Stretch)
            .separator()
            .option(CmdZoomIn, L"Zoom in")
            .option(CmdZoomOut, L"Zoom out")
            .option(CmdZoomReset, L"Reset zoom");
        menu.submenu(L"View / zoom", std::move(view));

        PanelMenu audioTracks;
        const std::size_t listedTracks = appendAudioTrackMenu(audioTracks, index);
        menu.submenu(L"Audio track", std::move(audioTracks), listedTracks > 1);
        PanelMenu audioSync;
        appendPaneAudioSyncMenu(audioSync, index);
        menu.submenu(L"Audio sync", std::move(audioSync), loaded);
        // Never greyed: with nothing on offer it is still where a subtitle
        // file is loaded from.
        PanelMenu subtitles;
        appendSubtitleMenu(subtitles, index);
        menu.submenu(L"Subtitles", std::move(subtitles));

        PanelMenu paneLoop;
        const double paneDuration = sources_[index] ? sources_[index]->duration() : 0.0;
        paneLoop.option(CmdPaneLoopSetA, L"Set A at current video time  (loops to the end until B is set)")
            .option(CmdPaneLoopSetB, L"Set B at current video time")
            .check(CmdPaneLoopToggle, L"Enable A-B loop", sourceLoopEnabled_[index], {},
                   loopArmed(sourceLoopA_[index], sourceLoopB_[index], paneDuration))
            .option(CmdPaneLoopClear, L"Clear A-B loop");
        menu.submenu(L"This video A-B loop", std::move(paneLoop));
        menu.separator();
    }

    const bool group = severalPanesLoaded();
    menu.option(CmdPlayPause, group ? (playbackIntended() ? L"Pause all" : L"Play all")
                                    : (playbackIntended() ? L"Pause" : L"Play"), false, shortcut(KeyAction::PlayPause));
    menu.option(CmdStop, group ? L"Stop all" : L"Stop");
    menu.check(CmdMute, group ? L"Mute all" : L"Mute", audio_.muted(), shortcut(KeyAction::Mute));
    PanelMenu audio;
    appendAudioMenu(audio);
    menu.submenu(L"Audio", std::move(audio));
    PanelMenu video;
    appendVideoMenu(video);
    menu.submenu(L"Video", std::move(video));
    PanelMenu order;
    for (int i = 0; i < kPlayOrderCount; ++i) {
        const PlayOrder value = clampPlayOrder(i);
        order.option(CmdPlayOrderFirst + static_cast<unsigned>(i), playOrderName(value), playOrder_ == value);
    }
    menu.submenu(L"When a video ends", std::move(order));
    PanelMenu masterLoop;
    masterLoop.option(CmdMasterLoopSetA, L"Set A at master time  (loops to the end until B is set)", false,
                      shortcut(KeyAction::LoopA))
        .option(CmdMasterLoopSetB, L"Set B at master time", false, shortcut(KeyAction::LoopB))
        .check(CmdMasterLoopToggle, L"Enable master A-B loop", masterLoopEnabled_, shortcut(KeyAction::LoopToggle),
               loopArmed(masterLoopA_, masterLoopB_, duration_))
        .option(CmdMasterLoopClear, L"Clear master loop");
    menu.submenu(L"Master A-B loop", std::move(masterLoop));
    if (severalPanesLoaded()) {
        // The two states changed while watching several videos. The other
        // states -- decoder, shader, presets -- stay in the sheet.
        PanelMenu arrangement;
        appendLayoutMenu(arrangement);
        menu.submenu(L"Arrangement", std::move(arrangement));
        PanelMenu seekBars;
        appendSeekModeMenu(seekBars);
        menu.submenu(L"Seek bars", std::move(seekBars));
    }
    menu.separator();

    // Away from any pane, the whole empty window too: a file, as O, and
    // Emby. Over a pane its own section opens them, into that pane.
    if (pane < 0) {
        menu.option(CmdOpenFiles, L"Open file...", false, shortcut(KeyAction::OpenFiles));
        menu.option(CmdOpenEmby, L"Open from Emby...", false, shortcut(KeyAction::Emby));
    }
    menu.option(CmdSessionOpen, L"Open session...", false, shortcut(KeyAction::OpenSession));
    menu.option(CmdSessionSave, L"Save session...", false, shortcut(KeyAction::SaveSession));
    menu.separator();
    menu.check(CmdFullscreen, L"Fullscreen", fullscreen_, shortcut(KeyAction::Fullscreen));
    menu.option(CmdSettings, L"Settings...", false, shortcut(KeyAction::Settings));
    // The pane's lists as they were listed go with the menu, for its command.
    openWindowMenu(std::move(menu), anchor,
                   [this, snapshot = takePaneMenuSnapshot(), pane](unsigned command) {
                       runPaneMenuCommand(snapshot, command, pane);
                   },
                   pane);
}

App::PaneMenuSnapshot App::takePaneMenuSnapshot() {
    PaneMenuSnapshot snapshot{std::move(subtitleMenuOptions_), subtitleMenuPane_, subtitleMenuSerial_,
                              std::move(audioMenuTracks_), audioMenuPane_, audioMenuSerial_,
                              std::move(embyFavoriteMenuOffer_)};
    clearPaneMenuSnapshot();
    return snapshot;
}

void App::clearPaneMenuSnapshot() {
    subtitleMenuOptions_.clear();
    subtitleMenuPane_ = -1;
    subtitleMenuSerial_ = 0;
    audioMenuTracks_.clear();
    audioMenuPane_ = -1;
    audioMenuSerial_ = 0;
    embyFavoriteMenuOffer_.reset();
}

void App::runPaneMenuCommand(const PaneMenuSnapshot& snapshot, unsigned command, int pane) {
    subtitleMenuOptions_ = snapshot.subtitleOptions;
    subtitleMenuPane_ = snapshot.subtitlePane;
    subtitleMenuSerial_ = snapshot.subtitleSerial;
    audioMenuTracks_ = snapshot.audioTracks;
    audioMenuPane_ = snapshot.audioPane;
    audioMenuSerial_ = snapshot.audioSerial;
    embyFavoriteMenuOffer_ = snapshot.favorite;
    handleContextCommand(command, pane);
    clearPaneMenuSnapshot();
}

std::size_t App::appendAudioTrackMenu(PanelMenu& menu, std::size_t pane) {
    const auto tracks = sources_[pane] ? sources_[pane]->audioTracks() : std::vector<AudioTrackInfo>{};
    const int activeTrack = paneAudioTrack(pane);
    // Kept for the command the menu returns, as the subtitle entries are:
    // the next video may open under the menu with other tracks.
    audioMenuTracks_.clear();
    audioMenuPane_ = static_cast<int>(pane);
    audioMenuSerial_ = subtitleSerial_[pane];
    for (std::size_t track = 0; track < tracks.size() && track < 32; ++track) {
        menu.option(CmdAudioTrackFirst + static_cast<unsigned>(track), audioTrackLabel(tracks[track]),
                    tracks[track].streamIndex == activeTrack);
        audioMenuTracks_.push_back(tracks[track].streamIndex);
    }
    return tracks.size();
}

std::optional<std::size_t> App::menuCommandPane(int menuPane, std::uint64_t menuSerial, int pane) {
    const auto index = static_cast<std::size_t>(pane);
    if (menuPane != pane || subtitleSerial_[index] == menuSerial) return index;
    // A swap moves the serial with the video.
    if (const auto found = subtitlePaneWithSerial(menuSerial)) return found;
    showNotice(L"The video changed while the menu was open; choose again");
    return std::nullopt;
}

void App::appendEmbyFavoriteMenu(PanelMenu& menu, std::size_t pane) const {
    embyFavoriteMenuOffer_.reset();
    if (!embyPaneAccountMatches(pane) || embyPanes_[pane]->itemId.empty()) return;
    const auto& state = *embyPanes_[pane];
    // Greyed until the item reply has said what the mark is, and while a
    // change to it is on its way.
    const bool known = state.favorite.has_value();
    const bool asking = embyFavoriteAsking_.contains(state.itemId);
    menu.option(CmdEmbyFavorite, known && *state.favorite ? L"Remove from Emby favorites" : L"Add to Emby favorites",
                false, {}, known && !asking);
    if (known && !asking) {
        embyFavoriteMenuOffer_ = EmbyFavoriteOffer{embyAccountSerial_, static_cast<int>(pane), state.itemId,
                                                   state.title, !*state.favorite};
    }
}

void App::handleContextCommand(unsigned command, int pane) {
    const bool validPane = pane >= 0 && pane < static_cast<int>(kMaxPanes);
    const std::size_t index = validPane ? static_cast<std::size_t>(pane) : 0;
    const auto changePaneLoopMapping = [&](const auto& change) {
        // A-B enable/disable can move an already repeating source by many
        // cycles. Compare both sides at one clock sample, then replace the
        // decoder generations instead of making them chase from the old
        // mapped position. The helper also rebuilds an active global barrier.
        const double timeline = clock_.position();
        const double before = mappedSourceTime(index, timeline);
        double current = before;
        if (sources_[index] && sources_[index]->duration() > 0.0) {
            current = std::clamp(current, 0.0, sources_[index]->duration());
        }
        change(current);
        realignPaneAfterMappingChange(index, timeline, before);
    };
    if (handleAudioCommand(command, pane)) return;
    if (handleVideoCommand(command)) return;
    if (command == CmdOpenPane && validPane) openFileForPane(index);
    else if (command == CmdOpenFiles) openFilesDialog();
    else if (command == CmdOpenEmby) embyOpenBrowser();
    else if (command == CmdEmbyFavorite && validPane) {
        // The item the menu offered, for the account it was offered under,
        // not whatever the pane has gone on to since.
        if (embyFavoriteMenuOffer_ && embyFavoriteMenuOffer_->pane == pane &&
            embyFavoriteMenuOffer_->account == embyAccountSerial_) {
            const auto offer = *embyFavoriteMenuOffer_;
            embySetFavorite(offer.itemId, offer.title, offer.wanted);
        }
    }
    else if (validPane && (command == CmdSubtitleOff || (command >= CmdSubtitleFirst && command <= CmdSubtitleLast) ||
                           command == CmdSubtitleCycle || command == CmdSubtitleLoad ||
                           command == CmdSubtitleEarlier || command == CmdSubtitleLater ||
                           command == CmdSubtitleSyncReset)) {
        // For the video the menu was built for, not whatever the pane has
        // gone on to under it; dropped when that video has gone.
        if (const auto target = menuCommandPane(subtitleMenuPane_, subtitleMenuSerial_, pane)) {
            const std::size_t at = *target;
            if (command == CmdSubtitleOff) {
                selectSubtitle(at, {}, true);
                showNotice(L"Subtitles off");
            } else if (command >= CmdSubtitleFirst && command <= CmdSubtitleLast) {
                // The entry as the menu listed it, not as the pane lists it now.
                const auto options = subtitleMenuPane_ == pane ? subtitleMenuOptions_ : subtitleOptions(at);
                const std::size_t choice = command - CmdSubtitleFirst;
                if (choice < options.size() && options[choice].usable) {
                    selectSubtitle(at, options[choice].what, true);
                    showNotice(L"Subtitles: " + options[choice].label);
                }
            }
            else if (command == CmdSubtitleCycle) cycleSubtitle(at);
            else if (command == CmdSubtitleLoad) loadSubtitleDialog(at);
            else if (command == CmdSubtitleEarlier) nudgeSubtitleDelay(at, -0.5);
            else if (command == CmdSubtitleLater) nudgeSubtitleDelay(at, 0.5);
            else resetSubtitleDelay(at);
        }
    }
    else if (validPane && command >= CmdAudioTrackFirst && command <= CmdAudioTrackLast) {
        // The stream the menu listed, of the video it was listed for.
        const std::size_t track = command - CmdAudioTrackFirst;
        if (audioMenuPane_ == pane) {
            const auto target = menuCommandPane(audioMenuPane_, audioMenuSerial_, pane);
            if (target && track < audioMenuTracks_.size()) selectAudioTrack(*target, audioMenuTracks_[track]);
        } else {
            const auto tracks = sources_[index] ? sources_[index]->audioTracks() : std::vector<AudioTrackInfo>{};
            if (track < tracks.size()) selectAudioTrack(index, tracks[track].streamIndex);
        }
    }
    else if (command == CmdClosePane && validPane) closePane(index);
    else if (command == CmdSelectAudio && validPane) toggleAudioPane(index);
    else if (command == CmdPaneMute && validPane) togglePaneMute(index);
    else if (command == CmdResetPaneOffset && validPane) resetPaneOffset(index);
    else if (command == CmdTogglePanePause && validPane) toggleSourcePause(index);
    else if (command == CmdTogglePaneRepeat && validPane) {
        activateChip(index, PaneChip::Repeat);
    }
    else if (command == CmdToggleSolo && validPane) toggleSolo(pane);
    else if (validPane && command >= CmdSpeed025 && command <= CmdSpeed200) {
        constexpr std::array<double, 5> rates{0.25, 0.5, 1.0, 1.5, 2.0};
        const double rate = rates[command - CmdSpeed025];
        if (rate == playbackRates_[index]) return;
        const double held = currentSourceTime(index);
        // A paused pane resumes from its held time, which rebuilds the
        // adjustment then; a running one keeps its time or, still waiting,
        // the master time it starts at.
        syncAdjustments_[index] = sourcePaused_[index]
            ? held - (clock_.position() - startDelays_[index]) * rate
            : rateChangeAdjustment(clock_.position(), startDelays_[index],
                                   syncAdjustments_[index], playbackRates_[index], rate, held);
        playbackRates_[index] = rate;
        // The seek below starts the sound's next generation at the new tempo.
        syncPaneAudioTempo(index);
        sourcePausedTimes_[index] = held;
        lastVideoMappedTimes_[index] = held;
        if (seekBarrier_.active()) seekAbsolute(clock_.position());
        else if (sources_[index]) {
            sources_[index]->setAudioEnabled(false);
            if (sourceInitialAlignmentPending_[index]) {
                sourceProvisionalTargets_[index] = held;
            }
            sources_[index]->requestSeek(held);
            const auto audioStatus = sources_[index]->audioDecodeStatus();
            audio_.flush(index, audioStatus.generation);
            const bool useAudio = shouldOutputAudio(
                index, clock_.position(), clock_.isPlaying());
            sources_[index]->setAudioEnabled(useAudio);
            if (useAudio) audio_.play(index);
        }
        if (audioPaneEnabled(index)) audio_.setRate(index, static_cast<float>(playbackRates_[index]));
        std::wostringstream notice;
        notice << L"V" << (positionForPane(index) + 1) << L"  "
               << std::setprecision(3) << playbackRates_[index] << L"x";
        showNotice(notice.str());
    } else if (validPane && command >= CmdViewFit && command <= CmdViewStretch) {
        paneViews_[index].mode = static_cast<ViewMode>(command - CmdViewFit);
        showNotice(L"V" + std::to_wstring(positionForPane(index) + 1) + L"  " +
                   viewModeName(paneViews_[index].mode));
    } else if (validPane && (command == CmdZoomIn || command == CmdZoomOut || command == CmdZoomReset)) {
        auto& zoom = paneViews_[index].zoom;
        if (command == CmdZoomIn) zoom = std::min(4.0F, zoom + 0.25F);
        else if (command == CmdZoomOut) zoom = std::max(1.0F, zoom - 0.25F);
        else zoom = 1.0F;
        std::wostringstream notice;
        notice << L"V" << (positionForPane(index) + 1) << L"  zoom "
               << std::setprecision(3) << zoom << L"x";
        showNotice(notice.str());
    }
    else if (command == CmdPaneLoopSetA && validPane) {
        // Setting A arms the loop at once: with no B it runs to the end of
        // the video and back to A. A B set later narrows it.
        const double paneDuration = sources_[index] ? sources_[index]->duration() : 0.0;
        changePaneLoopMapping([&](double current) {
            sourceLoopA_[index] = current;
            sourceLoopEnabled_[index] = loopArmed(sourceLoopA_[index], sourceLoopB_[index], paneDuration);
        });
        const bool toEnd = !validLoop(sourceLoopA_[index], sourceLoopB_[index]);
        showNotice(L"V" + std::to_wstring(positionForPane(index) + 1) + L"  loop A  " +
                   formatTime(sourceLoopA_[index]) +
                   (sourceLoopEnabled_[index] ? (toEnd ? L"  →  end" : L"  ·  loop on") : L""));
    } else if (command == CmdPaneLoopSetB && validPane) {
        const double paneDuration = sources_[index] ? sources_[index]->duration() : 0.0;
        changePaneLoopMapping([&](double current) {
            sourceLoopB_[index] = current;
            sourceLoopEnabled_[index] = loopArmed(sourceLoopA_[index], sourceLoopB_[index], paneDuration);
        });
        showNotice(L"V" + std::to_wstring(positionForPane(index) + 1) + L"  loop B  " +
                   formatTime(sourceLoopB_[index]) + (sourceLoopEnabled_[index] ? L"  ·  loop on" : L""));
    } else if (command == CmdPaneLoopToggle && validPane &&
               loopArmed(sourceLoopA_[index], sourceLoopB_[index],
                         sources_[index] ? sources_[index]->duration() : 0.0)) {
        changePaneLoopMapping([&](double) {
            sourceLoopEnabled_[index] = !sourceLoopEnabled_[index];
        });
    } else if (command == CmdPaneLoopClear && validPane) {
        changePaneLoopMapping([&](double) {
            sourceLoopEnabled_[index] = false;
            sourceLoopA_[index] = sourceLoopB_[index] = 0.0;
        });
    } else if (command == CmdMasterLoopSetA) {
        setMasterLoopA(clock_.position());
    } else if (command == CmdMasterLoopSetB) {
        setMasterLoopB(clock_.position());
    } else if (command == CmdMasterLoopToggle && loopArmed(masterLoopA_, masterLoopB_, duration_)) {
        masterLoopEnabled_ = !masterLoopEnabled_;
        showNotice(masterLoopEnabled_ ? L"Master A-B loop on" : L"Master A-B loop off");
    } else if (command == CmdMasterLoopClear) {
        masterLoopEnabled_ = false; masterLoopA_ = masterLoopB_ = 0.0;
        showNotice(L"Master A-B loop cleared");
    } else if (command == CmdPlayPause) togglePlayback();
    else if (command == CmdStop) stopPlayback();
    else if (command == CmdMute) toggleMute();
    else if (command == CmdToggleExpanded && validPane) {
        const auto active = activePanes();
        const auto activeCount = static_cast<std::size_t>(
            std::count(active.begin(), active.end(), true));
        if (supportsExpandedPaneCount(activeCount)) {
            expandedPane_ = expandedPane_ == pane ? -1 : pane;
            if (expandedPane_ >= 0) {
                layoutMode_ = LayoutMode::PortraitStack;
            }
            soloPane_ = -1;
            layoutHoverControls();
        }
    }
    else if (command >= CmdLayoutGrid && command <= CmdLayoutLandscape) {
        chooseLayout(static_cast<LayoutMode>(command - CmdLayoutGrid));
    }
    else if (command == CmdSeekLinked) chooseSeekMode(SeekMode::Linked);
    else if (command == CmdSeekIndependent) chooseSeekMode(SeekMode::Independent);
    else if (command == CmdSessionOpen) loadSessionDialog();
    else if (command == CmdSessionSave) saveSessionDialog();
    else if (command == CmdFullscreen) toggleFullscreen();
    else if (command == CmdPreviousFile) openAdjacentFile(-1, pane);
    else if (command == CmdNextFile) openAdjacentFile(1, pane);
    else if (command == CmdSettings) toggleSettingsPanel();
    else if (command >= CmdPlayOrderFirst && command <= CmdPlayOrderLast) {
        setPlayOrder(clampPlayOrder(static_cast<int>(command - CmdPlayOrderFirst)));
    }
    updateHoverControls();
    updateControls();
    updateTitle();
    scheduleAppSettingsSave();
}

void App::loadShaderDialog() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter =
        L"PotPlayer pixel shader (*.txt)\0*.txt\0HLSL pixel shader (*.hlsl)\0*.hlsl\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        const bool deferShader = deviceRecoveryPending_ || renderer_.deviceLost();
        if (!deferShader && !renderer_.loadPixelShader(path)) {
            MessageBoxA(window_, renderer_.error().c_str(), "Pixel shader error", MB_ICONERROR);
        } else {
            customShaderPath_ = path;
            customShaderUnloaded_ = false;
            scheduleAppSettingsSave();
        }
    }
}

bool App::applyShaderPreset(ShaderPreset preset) {
    const bool deferShader = deviceRecoveryPending_ || renderer_.deviceLost();
    if (!deferShader && !renderer_.setShaderPreset(preset)) {
        MessageBoxA(window_, renderer_.error().c_str(),
                    "Pixel shader error", MB_ICONERROR);
        return false;
    }
    shaderPreset_ = preset;
    customShaderPath_.clear();
    customShaderUnloaded_ = false;
    scheduleAppSettingsSave();
    return true;
}

// The chosen shader, compiled again on a new device. A custom file that
// cannot be read now -- a share not back yet after sleep, a drive not in --
// stays the choice and is not saved away: the preset runs meanwhile, the
// Shader page says so, and the next start or rebuild tries the file again.
// The frame loop reaches this through device recovery, so nothing here
// opens a box: a modal loop would run the tick again inside it.
void App::restoreShader() {
    customShaderUnloaded_ = false;
    if (!customShaderPath_.empty()) {
        if (renderer_.loadPixelShader(customShaderPath_)) return;
        customShaderUnloaded_ = true;
        appendDiagnostic("Custom pixel shader not loaded: " + renderer_.error() + "; running the preset");
        showNotice(L"Shader file not loaded; the preset runs until it can be read");
    }
    if (!renderer_.setShaderPreset(shaderPreset_)) {
        appendDiagnostic("Pixel shader preset not loaded: " + renderer_.error());
        if (shaderPreset_ != ShaderPreset::Normal) renderer_.setShaderPreset(ShaderPreset::Normal);
    }
}


void App::changeDecodeMode(DecodeMode mode) {
    if (mode == decodeMode_) return;
    if (deviceRecoveryPending_) {
        // No decoder/device exists to reopen yet. Preserve each deferred
        // pane's queue and muted-add identity for the eventual recovery.
        decodeMode_ = mode;
        updateTitle();
        updateControls();
        return;
    }
    const double position = clock_.position();
    const bool wasPlaying = playbackIntended();
    cancelSeekBarrier();
    clock_.pause();
    for (auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }
    audio_.flush();
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) retireSource(pane);
    sourceInitialAlignmentPending_.fill(false);
    sourceProvisionalTargets_.fill(0.0);
    decodeMode_ = mode;
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        if (!paths_[pane].empty()) openSource(pane, paths_[pane], false);
    }
    seekAbsolute(position);
    // During a failed-device retry seekAbsolute updates the saved target and
    // the original intent is already retained; toggling would invert it.
    if (wasPlaying && !deviceRecoveryPending_) togglePlayback();
}

// O and the empty window's Open file. Into an empty window the videos open
// as a launch from Explorer does (openExternalDeck): one deck from zero,
// started together; beside others they join the deck as it is, playing or
// paused.
void App::openFilesDialog() {
    std::vector<std::wstring> files;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return;
    }
    DWORD options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    const COMDLG_FILTERSPEC filters[] = {
        {L"Video files", L"*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.ts;*.wmv;*.flv"},
        {L"All files", L"*.*"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    if (SUCCEEDED(dialog->Show(window_))) {
        IShellItemArray* results = nullptr;
        if (SUCCEEDED(dialog->GetResults(&results))) {
            DWORD count{};
            results->GetCount(&count);
            for (DWORD index = 0; index < count && index < kMaxPanes; ++index) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(results->GetItemAt(index, &item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        files.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            results->Release();
        }
    }
    dialog->Release();
    if (files.empty()) return;
    if (anyPaneLoaded()) {
        addFiles(files);
        return;
    }
    loadFiles(files);
    // loadFiles leaves the deck paused at zero; this is Space's start.
    togglePlayback();
}

// Which pane a keyboard command applies to when it did not come from the
// popup menu, where the pane is explicit. Solo makes the choice unambiguous;
// otherwise the pane under the pointer is the one being looked at, which is
// the same rule the per-pane hover buttons already follow.
int App::commandPane() const {
    if (perPaneTimelines()) return static_cast<int>(embyPlaybackTarget());
    if (soloPane_ >= 0 && soloPane_ < static_cast<int>(kMaxPanes) &&
        paneLogicallyLoaded(static_cast<std::size_t>(soloPane_))) {
        return soloPane_;
    }
    if (lastPointerPane_ >= 0 && lastPointerPane_ < static_cast<int>(kMaxPanes) &&
        paneLogicallyLoaded(static_cast<std::size_t>(lastPointerPane_))) {
        return lastPointerPane_;
    }
    return -1;
}

void App::openAdjacentFile(int step, int pane) {
    // A key names no pane. The only video is the one it steps, as the
    // bar's buttons do, though the pointer has not been on it: a media
    // key is pressed away from the mouse.
    if (pane < 0) pane = commandPane();
    if (pane < 0) pane = singleLoadedPane();
    if (pane < 0 || pane >= static_cast<int>(kMaxPanes)) return;
    const auto index = static_cast<std::size_t>(pane);
    if (paths_[index].empty()) return;
    if (emby::isLocator(paths_[index])) {
        embyOpenAdjacent(index, step);
        return;
    }

    if (localPanes_[index]) {
        const auto queue = localPanes_[index]->queue;
        if (queue.size() < 2) return;
        const int position = localEntryIndex(queue, paths_[index]);
        const int next = stepFileIndex(position, static_cast<int>(queue.size()), step);
        if (next < 0) return;
        localPlayItem(queue[static_cast<std::size_t>(next)].path, queue, index);
        return;
    }

    std::vector<std::wstring> files;
    int position = -1;
    if (!siblingFiles(paths_[index], files, position)) {
        waitForSiblingFolder(index, step, false);
        return;
    }
    stepToSiblingFile(index, files, position, step);
}

void App::stepToSiblingFile(std::size_t index, const std::vector<std::wstring>& files, int position, int step) {
    if (files.size() < 2) return;
    const int pane = static_cast<int>(index);
    const int next = stepFileIndex(position, static_cast<int>(files.size()), step);
    if (next < 0) return;
    if (perPaneTimelines()) {
        std::vector<LocalEntry> queue;
        for (const auto& file : files) queue.push_back(LocalEntry{file});
        localPlayItem(files[static_cast<std::size_t>(next)], queue, index);
        return;
    }

    // PageUp/PageDown can arrive while a synchronized seek is still waiting
    // on the source being replaced. The barrier records generations from the
    // old VideoSource, so retaining it would make the new decoder wait for an
    // impossible generation until the timeout. Rebuild it around the same
    // timeline target, just like dialog and drag-and-drop replacement do.
    // The only video is the timeline, so its next file starts from zero.
    const bool onlyVideo = singleLoadedPane() == pane;
    const bool resume = playbackIntended();
    const bool resumeInterruptedBarrier =
        seekBarrier_.active() && seekBarrier_.resume();
    const double interruptedPosition = clock_.position();
    if (seekBarrier_.active()) cancelSeekBarrier();
    openSource(index, files[static_cast<std::size_t>(next)]);
    if (onlyVideo) {
        restartReplacedOnlyVideo(resume);
    } else if (resumeInterruptedBarrier) {
        beginSynchronizedSeek(interruptedPosition, false, true);
    }
    showNotice(L"V" + std::to_wstring(positionForPane(index) + 1) + L"  " +
               std::filesystem::path(files[static_cast<std::size_t>(next)]).filename().wstring());
    layoutHoverControls();
    updateHoverControls();
    updateControls();
    updateTitle();
}

bool App::siblingFiles(const std::wstring& path, std::vector<std::wstring>& files, int& position) const {
    files.clear();
    position = -1;
    // A stream has no folder to walk.
    if (path.empty() || emby::isLocator(path) || path.find(L"://") != std::wstring::npos) return true;
    const std::wstring directory = localDirectory(path);
    if (directory.empty()) return true;
    std::vector<LocalEntry> entries;
    if (localListCovers(path) && !localList_.empty()) {
        // The list the sheet shows is the list that is walked, every video
        // under its folder when it lists those.
        entries = localList_;
    } else {
        // Read and found empty counts as unread: the share may be back.
        const auto found = std::find_if(siblingFolders_.begin(), siblingFolders_.end(),
            [&](const SiblingFolder& folder) {
                return folder.listed && !folder.entries.empty() && sameLocalDirectory(folder.directory, directory);
            });
        if (found == siblingFolders_.end()) return false;
        entries = found->entries;
        orderLocalEntries(entries, emby::sortKeyFromIndex(embyBrowser_.sort), embyBrowser_.descending,
                          sessionSeed_ ^ std::hash<std::wstring>{}(directory));
    }
    // File names rather than whole paths: the stored path came from a dialog
    // or a drop and need not be spelled the way the directory listing is.
    position = localEntryIndex(entries, path);
    files.reserve(entries.size());
    for (auto& entry : entries) files.push_back(std::move(entry.path));
    return true;
}

bool App::playlistPlace(std::size_t pane, int& position, std::size_t& count) const {
    position = -1;
    count = 0;
    if (pane >= paths_.size() || paths_[pane].empty()) return false;
    const std::wstring& path = paths_[pane];
    if (emby::isLocator(path)) {
        // The page it was chosen on; past its ends the server's neighbours
        // are asked for at the step, which is no list to count.
        if (!embyPanes_[pane]) return false;
        count = embyPanes_[pane]->queue.items.size();
        position = embyPlaylistIndex(embyPanes_[pane]->itemId, pane);
        return count > 0;
    }
    if (localPanes_[pane]) {
        count = localPanes_[pane]->queue.size();
        position = localEntryIndex(localPanes_[pane]->queue, path);
        return count > 0;
    }
    // The F6 list without copying it: the title asks this a few times a
    // second, and the list can hold thousands.
    if (localListCovers(path) && !localList_.empty()) {
        count = localList_.size();
        position = localEntryIndex(localList_, path);
        return true;
    }
    std::vector<std::wstring> files;
    if (!siblingFiles(path, files, position)) return false;
    count = files.size();
    return count > 0;
}

void App::waitForSiblingFolder(std::size_t pane, int step, bool atEnd) {
    if (pane >= paths_.size() || paths_[pane].empty()) return;
    requestSiblingFolder(paths_[pane]);
    const ULONGLONG now = GetTickCount64();
    auto& pending = pendingFolderStep_;
    if (pending && pending->path == paths_[pane] && pending->mediaSerial == subtitleSerial_[pane] &&
        (pending->mediaSerial != 0 || pending->pane == pane)) {
        // Presses add up to one net step; the end is remembered beside them.
        pending->pane = pane;
        if (step != 0) {
            pending->step += step;
            pending->asked = now;
        }
        pending->atEnd = pending->atEnd || atEnd;
    } else {
        pending = PendingFolderStep{pane, paths_[pane], subtitleSerial_[pane], step, atEnd, now};
    }
    showNotice(L"Reading the folder\x2026");
}

void App::runPendingFolderStep() {
    if (!pendingFolderStep_) return;
    const PendingFolderStep pending = *pendingFolderStep_;
    // The video may have moved to another pane by a swap meanwhile.
    std::size_t pane = pending.pane;
    if (pending.mediaSerial != 0) {
        const auto found = subtitlePaneWithSerial(pending.mediaSerial);
        if (!found) {
            pendingFolderStep_.reset();
            return;
        }
        pane = *found;
    }
    // Closed, replaced, or since given an F6 or Emby queue of its own.
    if (pane >= paths_.size() || paths_[pane] != pending.path || localPanes_[pane] || embyPanes_[pane]) {
        pendingFolderStep_.reset();
        return;
    }
    std::vector<std::wstring> files;
    int position = -1;
    if (!siblingFiles(pending.path, files, position)) {
        // Read, and nothing there: a share gone away, or the folder emptied.
        const std::wstring directory = localDirectory(pending.path);
        const bool reading = (sameLocalDirectory(directory, localDirectory_) && localListLoading_) ||
            std::any_of(siblingFolders_.begin(), siblingFolders_.end(), [&](const SiblingFolder& folder) {
                return folder.listing && sameLocalDirectory(folder.directory, directory);
            });
        if (!reading) {
            pendingFolderStep_.reset();
            showNotice(L"The folder could not be read");
        }
        return;
    }
    pendingFolderStep_.reset();
    const bool stepStillWanted = pending.step != 0 && GetTickCount64() - pending.asked <= kFolderStepWaitMs;
    if (stepStillWanted) {
        stepToSiblingFile(pane, files, position, pending.step);
        return;
    }
    // The end waited, paused, for the folder. It goes on only if this is
    // still the only video, held at its end, and the order still goes on.
    if (pending.atEnd && !perPaneTimelines() && singleLoadedPane() == static_cast<int>(pane) &&
        !clock_.isPlaying() && duration_ > 0.0 && clock_.position() + kSourceEndEpsilon >= duration_ &&
        playOrder_ != PlayOrder::PlayOne && playOrder_ != PlayOrder::RepeatOne) {
        advanceToSiblingFile(pane, files, position);
    }
}

// A pane counts as taken from the moment it has a path: an Emby item still
// being resolved has no source yet but is about to play.
int App::singleLoadedPane() const {
    int single = -1;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!paneLogicallyLoaded(pane) && paths_[pane].empty()) continue;
        if (single >= 0) return -1;
        single = static_cast<int>(pane);
    }
    return single;
}

SeekMode App::activeSeekMode() const {
    return effectiveSeekMode(paneOrigins(), seekMode_);
}

// Loaded as singleLoadedPane counts it: a pane still opening is loaded.
bool App::severalPanesLoaded() const {
    int count = 0;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (paneLogicallyLoaded(pane) || !paths_[pane].empty()) ++count;
    }
    return count >= 2;
}

void App::chooseLayout(LayoutMode mode) {
    layoutMode_ = mode;
    soloPane_ = -1;
    showNotice(L"Layout: " + std::wstring(layoutModeName(mode)));
    scheduleAppSettingsSave();
}

void App::chooseSeekMode(SeekMode choice) {
    if (perPaneTimelines()) {
        if (choice != browserSeekMode_) {
            // Independent wraps each repeating video alone and Linked none:
            // one already wrapped stays on its frame, and one held at its end
            // for the others starts over as Independent would have had it.
            const double timeline = clock_.position();
            PaneArray<double> shown{};
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) shown[pane] = mappedSourceTime(pane, timeline);
            const bool leavingLinked = linkedBrowserSeekBars();
            browserSeekMode_ = choice;
            for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
                if (!sources_[pane]) continue;
                const bool heldAtEnd = (embyPanes_[pane] && embyPanes_[pane]->endHandled) ||
                                       (localPanes_[pane] && localPanes_[pane]->endHandled);
                if (leavingLinked && heldAtEnd && repeatsWithLinkedVideos(pane)) {
                    seekPaneTo(pane, 0.0);
                } else if (timelineMappingChanged(shown[pane], mappedSourceTime(pane, timeline))) {
                    syncAdjustments_[pane] = shown[pane] - (timeline - startDelays_[pane]) * playbackRates_[pane];
                }
            }
            browserSeekAligned_ = false;
            showNotice(choice == SeekMode::Linked
                ? L"Linked: click a seek bar to align all videos"
                : L"Browser seek bars independent");
            if (videoWindow_) {
                RECT client{};
                GetClientRect(videoWindow_, &client);
                refreshChromeGeometry(client);
            }
        }
    } else if (choice != seekMode_) {
        seekMode_ = choice;
        seekAbsolute(clock_.position());
        showNotice(seekMode_ == SeekMode::Linked ? L"Seek bars linked" : L"Seek bars independent");
    }
    scheduleAppSettingsSave();
}

void App::appendLayoutMenu(PanelMenu& menu) const {
    for (unsigned i = 0; i < 6; ++i) {
        const auto mode = static_cast<LayoutMode>(i);
        menu.option(CmdLayoutGrid + i, layoutModeName(mode), layoutMode_ == mode, shortcut(arrangementKeyAction(i)));
    }
}

void App::appendSeekModeMenu(PanelMenu& menu) const {
    const SeekMode current = perPaneTimelines() ? browserSeekMode_ : seekMode_;
    menu.option(CmdSeekLinked, L"Linked  (one seek bar moves every video)", current == SeekMode::Linked);
    menu.option(CmdSeekIndependent, L"Independent  (each video seeks alone)", current == SeekMode::Independent);
}

void App::showLayoutMenu() {
    PanelMenu menu;
    appendLayoutMenu(menu);
    openWindowMenu(std::move(menu), WindowMenuAnchor{{}, static_cast<int>(BarItem::Layout), -1},
                   [this](unsigned command) { handleContextCommand(command, -1); });
}

bool App::linkedBrowserSeekBars() const {
    if (!perPaneTimelines() || browserSeekMode_ != SeekMode::Linked) return false;
    const auto origins = paneOrigins();
    int videoCount = 0;
    for (const auto origin : origins) {
        if (origin != PaneOrigin::Empty && origin != PaneOrigin::EmbyPhoto) ++videoCount;
    }
    return videoCount > 1;
}

int App::linkedBrowserReferencePane() const {
    const auto origins = paneOrigins();
    const auto usable = [&](std::size_t pane) {
        return pane < sources_.size() && origins[pane] != PaneOrigin::Empty &&
            origins[pane] != PaneOrigin::EmbyPhoto && sources_[pane] &&
            sources_[pane]->ready() && sources_[pane]->duration() > 0.0;
    };
    const auto selected = embyPlaybackTarget();
    if (usable(selected)) return static_cast<int>(selected);
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (usable(pane)) return static_cast<int>(pane);
    }
    return -1;
}

PaneOrigins App::paneOrigins() const {
    PaneOrigins origins{};
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        const bool loaded = sources_[pane] || !paths_[pane].empty();
        if (localPanes_[pane] && loaded) {
            origins[pane] = PaneOrigin::LocalBrowser;
        } else if (embyPanes_[pane]) {
            origins[pane] = embyPanes_[pane]->photo ? PaneOrigin::EmbyPhoto : PaneOrigin::EmbyVideo;
        } else if (emby::isLocator(paths_[pane])) {
            // A restored locator already needs its own timeline while its
            // metadata and stream are still being resolved.
            origins[pane] = PaneOrigin::EmbyVideo;
        } else if (loaded) {
            origins[pane] = PaneOrigin::Manual;
        }
    }
    return origins;
}

bool App::perPaneTimelines() const {
    // Merely opening/refreshing F6 does not create a browser pane.
    return deckTimeline(paneOrigins()) == DeckTimeline::PerPane;
}

bool App::anyPaneLoaded() const {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (paneLogicallyLoaded(pane) || !paths_[pane].empty()) return true;
    }
    return false;
}

void App::pausePlaybackAtEnd() {
    clock_.pause();
    audio_.pause();
    for (auto& source : sources_) if (source) source->setAudioEnabled(false);
}

void App::finishMasterTimeline(bool sourceOpening, bool hasRepeatingSource) {
    // The only video paused on its own is held where it was while the clock
    // runs on: the clock passing its length is no end of it, and resuming
    // it carries on from the held frame.
    if (perPaneTimelines() || heldOnlyPane() >= 0) return;
    const auto endAction = masterTimelineEndAction(
        clock_.position(), duration_, clock_.isPlaying(), sourceOpening,
        masterLoopEnabled_ && loopArmed(masterLoopA_, masterLoopB_, duration_),
        hasRepeatingSource, repeatAll_);
    if (endAction != MasterTimelineEndAction::None && finishSingleVideo()) {
        // The only video ended and the playback order said what comes next.
    } else if (endAction == MasterTimelineEndAction::Restart) {
        seekAbsolute(0.0);
    } else if (endAction == MasterTimelineEndAction::Pause) {
        pausePlaybackAtEnd();
    }
}

int App::heldOnlyPane() const {
    if (perPaneTimelines()) return -1;
    const int single = singleLoadedPane();
    return single >= 0 && sourcePaused_[static_cast<std::size_t>(single)] ? single : -1;
}

bool App::finishSingleVideo() {
    // Emby queues advance per pane, including when this happens to be the
    // only video. They must never pause or restart the whole deck at EOF.
    if (perPaneTimelines()) return false;
    const int single = singleLoadedPane();
    if (single < 0) return false;
    if (playOrder_ == PlayOrder::RepeatOne) {
        seekAbsolute(0.0);
        return true;
    }
    // Paused at the end first: what opens next resumes on its own, and
    // nothing is asked of the same ended video every frame meanwhile.
    pausePlaybackAtEnd();
    if (playOrder_ != PlayOrder::PlayOne) advanceAtEnd(static_cast<std::size_t>(single));
    return true;
}

void App::finishEmbyPanes() {
    if (!clock_.isPlaying()) return;
    const double timeline = clock_.position();
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!embyPanes_[pane] || embyPanes_[pane]->photo || embyPanes_[pane]->resolving ||
            embyPanes_[pane]->endHandled || sourcePaused_[pane] || !sources_[pane] ||
            !sources_[pane]->ready() || !sources_[pane]->error().empty()) continue;
        const auto timing = paneTiming(pane);
        if (!paneIsActive(timing, timeline) || paneRepeats(timing, activeSeekMode())) continue;
        // The decoder can reach EOF while decoded frames are still queued.
        // Advance only when the source's presentation timeline reaches its
        // duration, using server metadata when the container has none.
        const double duration = timing.duration > 0.0 ? timing.duration
            : emby::secondsFromTicks(embyPanes_[pane]->runTimeTicks);
        if (duration > 0.0 && mappedSourceTime(pane, timeline) + kSourceEndEpsilon >= duration) {
            embyFinishPane(pane);
        }
    }
}

void App::embyFinishPane(std::size_t pane) {
    if (pane >= embyPanes_.size() || !embyPanes_[pane] || embyPanes_[pane]->photo ||
        embyPanes_[pane]->endHandled || !embyPaneAccountMatches(pane)) return;
    auto& state = *embyPanes_[pane];
    state.endHandled = true;
    // Hold only this pane at its end. A next-item request may fail, have no
    // neighbour, or name an item already open elsewhere; none may resume or
    // restart this ended source, or interrupt another pane's clock or voice.
    const double duration = sources_[pane] ? sources_[pane]->duration() : 0.0;
    sourcePausedTimes_[pane] = duration > 0.0 ? duration : currentSourceTime(pane);
    sourcePaused_[pane] = true;
    if (sources_[pane]) sources_[pane]->setAudioEnabled(false);
    audio_.flush(pane);
    detachPaneSeekBarrier(pane);
    embyReport(pane, "stopped");
    if (linkedBrowserSeekBars() && repeatsWithLinkedVideos(pane)) {
        // It waits here for the others: finishLinkedBrowserSet.
    } else if (playOrder_ == PlayOrder::RepeatOne) {
        sourcePaused_[pane] = false;
        seekPaneTo(pane, 0.0);
    } else if (playOrder_ != PlayOrder::PlayOne) {
        advanceAtEnd(pane);
    }
    updateControls();
    updateHoverControls();
    updateTitle();
}

void App::finishLocalPanes() {
    if (!clock_.isPlaying()) return;
    const double timeline = clock_.position();
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!localPanes_[pane] || localPanes_[pane]->endHandled || sourcePaused_[pane] ||
            !sources_[pane] || !sources_[pane]->ready() || !sources_[pane]->error().empty()) continue;
        const auto timing = paneTiming(pane);
        if (!paneIsActive(timing, timeline) || paneRepeats(timing, activeSeekMode())) continue;
        if (timing.duration > 0 &&
            mappedSourceTime(pane, timeline) + kSourceEndEpsilon >= timing.duration) localFinishPane(pane);
    }
}

void App::localFinishPane(std::size_t pane) {
    if (pane >= localPanes_.size() || !localPanes_[pane] || localPanes_[pane]->endHandled) return;
    localPanes_[pane]->endHandled = true;
    const double duration = sources_[pane] ? sources_[pane]->duration() : 0;
    sourcePausedTimes_[pane] = duration > 0 ? duration : currentSourceTime(pane);
    sourcePaused_[pane] = true;
    if (sources_[pane]) sources_[pane]->setAudioEnabled(false);
    audio_.flush(pane);
    detachPaneSeekBarrier(pane);
    if (linkedBrowserSeekBars() && repeatsWithLinkedVideos(pane)) {
        // It waits here for the others: finishLinkedBrowserSet.
    } else if (playOrder_ == PlayOrder::RepeatOne) {
        sourcePaused_[pane] = false;
        seekPaneTo(pane, 0.0, true);
    } else if (playOrder_ != PlayOrder::PlayOne) {
        advanceAtEnd(pane);
    }
    updateControls();
    updateHoverControls();
    updateTitle();
}

// Once no Linked video plays -- each held at its end, paused, or at the end
// of what it has -- and one held at its end repeats, they all start over as
// the rail sent to its start puts them: each at its offset, one below zero
// waiting, and the rail's time back at zero rather than counting on.
void App::finishLinkedBrowserSet() {
    if (!clock_.isPlaying() || seekBarrier_.active() || !linkedBrowserSeekBars()) return;
    const auto origins = paneOrigins();
    const double timeline = clock_.position();
    bool repeat = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
        if (!sources_[pane] || !sources_[pane]->ready() || !sources_[pane]->error().empty() ||
            sources_[pane]->duration() <= 0.0) return;
        if (!sourcePaused_[pane] &&
            mappedSourceTime(pane, timeline) + kSourceEndEpsilon < sources_[pane]->duration()) return;
        const bool ended = (embyPanes_[pane] && embyPanes_[pane]->endHandled) ||
                           (localPanes_[pane] && localPanes_[pane]->endHandled);
        repeat = repeat || (ended && repeatsWithLinkedVideos(pane));
    }
    if (repeat) alignBrowserPanesToTime(0.0, false);
}

void App::advanceAtEnd(std::size_t pane) {
    if (pane >= sources_.size() || paths_[pane].empty()) return;
    const auto random = static_cast<unsigned>(shuffleRandom_());
    if (emby::isLocator(paths_[pane])) {
        if (!embyPanes_[pane] || !embyPaneAccountMatches(pane)) return;
        const std::string current = embyPanes_[pane]->itemId;
        const int index = embyPlaylistIndex(current, pane);
        if (index < 0) {
            // Not chosen from a list: the server's next episode or file.
            embyOpenAdjacent(pane, 1, false);
            return;
        }
        const int next = playOrderNextIndex(playOrder_, index,
                                           static_cast<int>(embyPanes_[pane]->queue.items.size()), random);
        if (next < 0) {
            showNotice(L"End of the list");
            return;
        }
        embyPlayFromPlaylist(next, pane, false);
        return;
    }
    if (localPanes_[pane]) {
        const auto queue = localPanes_[pane]->queue;
        const int position = localEntryIndex(queue, paths_[pane]);
        const int next = playOrderNextIndex(playOrder_, position, static_cast<int>(queue.size()), random);
        if (next < 0) { showNotice(L"End of the folder"); return; }
        localPlayItem(queue[static_cast<std::size_t>(next)].path, queue, pane, false, false);
        return;
    }
    std::vector<std::wstring> files;
    int position = -1;
    if (!siblingFiles(paths_[pane], files, position)) {
        // Paused at the end already, so the wait costs nothing.
        waitForSiblingFolder(pane, 0, true);
        return;
    }
    advanceToSiblingFile(pane, files, position);
}

void App::advanceToSiblingFile(std::size_t pane, const std::vector<std::wstring>& files, int position) {
    const auto random = static_cast<unsigned>(shuffleRandom_());
    const int next = playOrderNextIndex(playOrder_, position, static_cast<int>(files.size()), random);
    if (next < 0) {
        showNotice(L"End of the folder");
        return;
    }
    const std::wstring& file = files[static_cast<std::size_t>(next)];
    if (seekBarrier_.active()) cancelSeekBarrier();
    openSource(pane, file);
    beginSynchronizedSeek(0.0, false, true);
    showNotice(std::filesystem::path(file).filename().wstring());
    layoutHoverControls();
    updateHoverControls();
    updateControls();
    updateTitle();
}

// As when the playback order advances at the end: one video is the timeline,
// and the time the replaced video had reached is not the new one's.
void App::restartReplacedOnlyVideo(bool resume) {
    if (resume && !deviceRecoveryPending_) {
        beginSynchronizedSeek(0.0, false, true);
    } else {
        // Paused, or between device-recovery attempts, where seekAbsolute
        // keeps the position the recovery will restore.
        seekAbsolute(0.0);
    }
}

void App::setPlayOrder(PlayOrder order) {
    playOrder_ = order;
    showNotice(std::wstring(L"Each browser video / only other video: ") + playOrderName(order));
    scheduleAppSettingsSave();
}

void App::toggleMasterLoop() {
    if (loopArmed(masterLoopA_, masterLoopB_, duration_)) {
        masterLoopEnabled_ = !masterLoopEnabled_;
        showNotice(masterLoopEnabled_ ? L"Master A-B loop on" : L"Master A-B loop off");
    } else {
        showNotice(L"Set A first  ( [ )");
    }
    updateTitle();
}

void App::openFileForPane(std::size_t pane) {
    if (pane >= sources_.size()) return;
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Video files\0*.mp4;*.mkv;*.mov;*.avi;*.webm;*.m4v;*.ts;*.wmv;*.flv\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) {
        const bool onlyVideo = singleLoadedPane() == static_cast<int>(pane);
        const bool resume = playbackIntended();
        const bool resumeInterruptedBarrier = seekBarrier_.active() && seekBarrier_.resume();
        const double interruptedPosition = clock_.position();
        if (seekBarrier_.active()) cancelSeekBarrier();
        openSource(pane, path);
        bool hasSelectedLoadedAudio = false;
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            hasSelectedLoadedAudio = hasSelectedLoadedAudio ||
                (paneLogicallyLoaded(index) && audioPaneEnabled(index));
        }
        if (!hasSelectedLoadedAudio) setAudioMask(1U << pane);
        if (onlyVideo) {
            restartReplacedOnlyVideo(resume);
        } else if (resumeInterruptedBarrier) {
            beginSynchronizedSeek(interruptedPosition, false, true);
        }
        updateControls();
        updateTitle();
    }
}

}  // namespace quaddeck
