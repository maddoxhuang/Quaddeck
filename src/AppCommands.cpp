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

void App::showContextMenu(int pane, POINT screenPoint) {
    contextPane_ = pane;
    contextMenuOpen_ = true;
    HMENU menu = CreatePopupMenu();
    if (!menu) { contextMenuOpen_ = false; contextPane_ = -1; return; }
    if (pane >= 0 && pane < static_cast<int>(kMaxPanes)) {
        const std::size_t index = static_cast<std::size_t>(pane);
        const bool loaded = paneLogicallyLoaded(index);
        AppendMenuW(menu, MF_STRING, CmdOpenPane, loaded ? L"Replace video..." : L"Open video...");
        AppendMenuW(menu, MF_STRING, CmdOpenEmby, L"Open from Emby...\tCtrl+E");
        AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), CmdPreviousFile,
                    L"Previous file in folder	Page Up");
        AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), CmdNextFile,
                    L"Next file in folder	Page Down");
        AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), CmdClosePane, L"Close video");
        AppendMenuW(menu, MF_STRING | (audioPaneEnabled(index) ? MF_CHECKED : 0) |
                    (loaded ? 0 : MF_GRAYED), CmdSelectAudio, L"Output this audio\t1-5");
        AppendMenuW(menu, MF_STRING | (audio_.paneMuted(index) ? MF_CHECKED : 0) |
                    (loaded ? 0 : MF_GRAYED), CmdPaneMute, L"Mute this video");
        AppendMenuW(menu, MF_STRING | (sources_[index] ? 0 : MF_GRAYED), CmdTogglePanePause,
                    sourcePaused_[index] ? L"Resume this video" : L"Pause this video");
        AppendMenuW(menu, MF_STRING | (sourceAutoRepeat_[index] ? MF_CHECKED : 0) |
                    (loaded ? 0 : MF_GRAYED), CmdTogglePaneRepeat,
                    L"Repeat this video (Independent mode)");
        AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), CmdToggleSolo,
                    soloPane_ == pane ? L"Return to grid" : L"Solo this video");
        AppendMenuW(menu, MF_STRING |
                    (sources_[index] && paneOffsetSeconds(index) != 0.0 ? 0 : MF_GRAYED),
                    CmdResetPaneOffset, L"Reset this video's offset");
        const auto active = activePanes();
        const auto activeCount = static_cast<std::size_t>(
            std::count(active.begin(), active.end(), true));
        const bool canFocusPane = supportsExpandedPaneCount(activeCount);
        const bool paneIsFocused = canFocusPane && expandedPane_ == pane;
        AppendMenuW(menu, MF_STRING | (paneIsFocused ? MF_CHECKED : 0) |
                    (canFocusPane ? 0 : MF_GRAYED),
                    CmdToggleExpanded,
                    paneIsFocused ? L"Use automatic focus"
                                  : L"Make this video the focus");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        HMENU speed = CreatePopupMenu();
        const auto speedItem = [&](unsigned id, double value, const wchar_t* text) {
            AppendMenuW(speed, MF_STRING | (std::abs(playbackRates_[index] - value) < 0.001 ? MF_CHECKED : 0), id, text);
        };
        speedItem(CmdSpeed025, 0.25, L"0.25x"); speedItem(CmdSpeed050, 0.5, L"0.5x");
        speedItem(CmdSpeed100, 1.0, L"1.0x"); speedItem(CmdSpeed150, 1.5, L"1.5x");
        speedItem(CmdSpeed200, 2.0, L"2.0x");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"Playback speed");

        HMENU view = CreatePopupMenu();
        AppendMenuW(view, MF_STRING | (paneViews_[index].mode == ViewMode::Fit ? MF_CHECKED : 0), CmdViewFit, L"Fit");
        AppendMenuW(view, MF_STRING | (paneViews_[index].mode == ViewMode::Fill ? MF_CHECKED : 0), CmdViewFill, L"Fill");
        AppendMenuW(view, MF_STRING | (paneViews_[index].mode == ViewMode::Stretch ? MF_CHECKED : 0), CmdViewStretch, L"Stretch");
        AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(view, MF_STRING, CmdZoomIn, L"Zoom in");
        AppendMenuW(view, MF_STRING, CmdZoomOut, L"Zoom out");
        AppendMenuW(view, MF_STRING, CmdZoomReset, L"Reset zoom");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View / zoom");

        HMENU audioTracks = CreatePopupMenu();
        const auto tracks = sources_[index] ? sources_[index]->audioTracks() : std::vector<AudioTrackInfo>{};
        const int activeTrack = sources_[index] ? sources_[index]->activeAudioTrack() : -1;
        for (std::size_t track = 0; track < tracks.size() && track < 32; ++track) {
            AppendMenuW(audioTracks, MF_STRING | (tracks[track].streamIndex == activeTrack ? MF_CHECKED : 0),
                        CmdAudioTrackFirst + static_cast<unsigned>(track), audioTrackLabel(tracks[track]).c_str());
        }
        AppendMenuW(menu, MF_POPUP | (tracks.size() > 1 ? 0 : MF_GRAYED),
                    reinterpret_cast<UINT_PTR>(audioTracks), L"Audio track");
        // Never greyed: with nothing on offer it is still where a subtitle
        // file is loaded from.
        HMENU subtitles = CreatePopupMenu();
        appendSubtitleMenu(subtitles, index);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(subtitles), L"Subtitles");

        HMENU paneLoop = CreatePopupMenu();
        const double paneDuration = sources_[index] ? sources_[index]->duration() : 0.0;
        AppendMenuW(paneLoop, MF_STRING, CmdPaneLoopSetA, L"Set A at current video time  (loops to the end until B is set)");
        AppendMenuW(paneLoop, MF_STRING, CmdPaneLoopSetB, L"Set B at current video time");
        AppendMenuW(paneLoop, MF_STRING | (sourceLoopEnabled_[index] ? MF_CHECKED : 0) |
                    (loopArmed(sourceLoopA_[index], sourceLoopB_[index], paneDuration) ? 0 : MF_GRAYED),
                    CmdPaneLoopToggle, L"Enable A-B loop");
        AppendMenuW(paneLoop, MF_STRING, CmdPaneLoopClear, L"Clear A-B loop");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(paneLoop), L"This video A-B loop");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }


    AppendMenuW(menu, MF_STRING, CmdPlayPause,
                playbackIntended() ? L"Pause\tSpace" : L"Play\tSpace");
    AppendMenuW(menu, MF_STRING, CmdStop, L"Stop");
    AppendMenuW(menu, MF_STRING | (audio_.muted() ? MF_CHECKED : 0), CmdMute, L"Mute\tM");
    HMENU order = CreatePopupMenu();
    for (int i = 0; i < kPlayOrderCount; ++i) {
        const PlayOrder value = clampPlayOrder(i);
        AppendMenuW(order, MF_STRING | (playOrder_ == value ? MF_CHECKED : 0),
                    CmdPlayOrderFirst + static_cast<unsigned>(i), playOrderName(value));
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(order), L"When each Emby video / the only local video ends");
    HMENU masterLoop = CreatePopupMenu();
    AppendMenuW(masterLoop, MF_STRING, CmdMasterLoopSetA, L"Set A at master time  (loops to the end until B is set)\t[");
    AppendMenuW(masterLoop, MF_STRING, CmdMasterLoopSetB, L"Set B at master time\t]");
    AppendMenuW(masterLoop, MF_STRING | (masterLoopEnabled_ ? MF_CHECKED : 0) |
                (loopArmed(masterLoopA_, masterLoopB_, duration_) ? 0 : MF_GRAYED), CmdMasterLoopToggle,
                L"Enable master A-B loop\tL");
    AppendMenuW(masterLoop, MF_STRING, CmdMasterLoopClear, L"Clear master loop");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(masterLoop), L"Master A-B loop");
    if (severalPanesLoaded()) {
        // The two states changed while watching several videos. The other
        // states -- decoder, shader, presets -- stay in the sheet.
        HMENU arrangement = CreatePopupMenu();
        appendLayoutMenu(arrangement);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(arrangement), L"Arrangement");
        HMENU seekBars = CreatePopupMenu();
        appendSeekModeMenu(seekBars);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(seekBars), L"Seek bars");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (pane < 0) AppendMenuW(menu, MF_STRING, CmdOpenEmby, L"Open from Emby...\tCtrl+E");
    AppendMenuW(menu, MF_STRING, CmdSessionOpen, L"Open session...\tCtrl+O");
    AppendMenuW(menu, MF_STRING, CmdSessionSave, L"Save session...\tCtrl+S");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), CmdFullscreen, L"Fullscreen\tAlt+Enter");
    AppendMenuW(menu, MF_STRING, CmdSettings, L"Settings...\tF5");
    const unsigned command = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, window_, nullptr);
    if (command) handleContextCommand(command, pane);
    DestroyMenu(menu);
    subtitleMenuOptions_.clear();
    subtitleMenuPane_ = -1;
    contextMenuOpen_ = false;
    contextPane_ = -1;
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
    if (command == CmdOpenPane && validPane) openFileForPane(index);
    else if (command == CmdOpenEmby) embyOpenBrowser();
    else if (command == CmdSubtitleShow) toggleSubtitlesShown();
    else if (command == CmdSubtitleOff && validPane) {
        selectSubtitle(index, {}, true);
        showNotice(L"Subtitles off");
    }
    else if (validPane && command >= CmdSubtitleFirst && command <= CmdSubtitleLast) {
        // The entry as the menu listed it, not as the pane lists it now.
        const auto options = subtitleMenuPane_ == pane ? subtitleMenuOptions_ : subtitleOptions(index);
        const std::size_t choice = command - CmdSubtitleFirst;
        if (choice < options.size() && options[choice].usable) {
            selectSubtitle(index, options[choice].what, true);
            showNotice(L"Subtitles: " + options[choice].label);
        }
    }
    else if (command == CmdSubtitleCycle && validPane) cycleSubtitle(index);
    else if (command == CmdSubtitleLoad && validPane) loadSubtitleDialog(index);
    else if (command == CmdSubtitleEarlier && validPane) nudgeSubtitleDelay(index, -0.5);
    else if (command == CmdSubtitleLater && validPane) nudgeSubtitleDelay(index, 0.5);
    else if (command == CmdSubtitleSyncReset && validPane) resetSubtitleDelay(index);
    else if (validPane && command >= CmdAudioTrackFirst && command <= CmdAudioTrackLast) {
        const auto tracks = sources_[index] ? sources_[index]->audioTracks() : std::vector<AudioTrackInfo>{};
        const std::size_t track = command - CmdAudioTrackFirst;
        if (track < tracks.size()) selectAudioTrack(index, tracks[track].streamIndex);
    }
    else if (command == CmdClosePane && validPane) closePane(index);
    else if (command == CmdSelectAudio && validPane) toggleAudioPane(index);
    else if (command == CmdPaneMute && validPane) togglePaneMute(index);
    else if (command == CmdResetPaneOffset && validPane) resetPaneOffset(index);
    else if (command == CmdTogglePanePause && validPane) toggleSourcePause(index);
    else if (command == CmdTogglePaneRepeat && validPane) {
        setSourceAutoRepeat(index, !sourceAutoRepeat_[index]);
    }
    else if (command == CmdToggleSolo && validPane) toggleSolo(pane);
    else if (validPane && command >= CmdSpeed025 && command <= CmdSpeed200) {
        constexpr std::array<double, 5> rates{0.25, 0.5, 1.0, 1.5, 2.0};
        const double held = currentSourceTime(index);
        playbackRates_[index] = rates[command - CmdSpeed025];
        const double natural = (clock_.position() - startDelays_[index]) * playbackRates_[index];
        syncAdjustments_[index] = held - natural;
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
    scheduleAppSettingsSave();
    return true;
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
    for (auto& source : sources_) source.reset();
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

void App::openFilesDialog() {
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
            std::vector<std::wstring> files;
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
            if (!files.empty()) {
                addFiles(files);
            }
        }
    }
    dialog->Release();
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
    if (pane < 0) pane = commandPane();
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

    int position = -1;
    const auto files = siblingFiles(paths_[index], position);
    if (files.size() < 2) return;
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

std::vector<std::wstring> App::siblingFiles(const std::wstring& path, int& position) const {
    position = -1;
    const std::wstring directory = localDirectory(path);
    std::vector<LocalEntry> entries;
    if (sameLocalDirectory(directory, localDirectory_) && !localList_.empty()) {
        // The list the sheet shows is the list that is walked.
        entries = localList_;
    } else {
        entries = scanLocalFolder(directory);
        orderLocalEntries(entries, emby::sortKeyFromIndex(embyBrowser_.sort), embyBrowser_.descending,
                          sessionSeed_ ^ std::hash<std::wstring>{}(directory));
    }
    // File names rather than whole paths: the stored path came from a dialog
    // or a drop and need not be spelled the way the directory listing is.
    position = localEntryIndex(entries, path);
    std::vector<std::wstring> files;
    files.reserve(entries.size());
    for (auto& entry : entries) files.push_back(std::move(entry.path));
    return files;
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
            browserSeekMode_ = choice;
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

void App::appendLayoutMenu(HMENU menu) const {
    for (unsigned i = 0; i < 6; ++i) {
        const auto mode = static_cast<LayoutMode>(i);
        AppendMenuW(menu, MF_STRING | (layoutMode_ == mode ? MF_CHECKED : 0), CmdLayoutGrid + i,
                    layoutModeName(mode));
    }
}

void App::appendSeekModeMenu(HMENU menu) const {
    const SeekMode current = perPaneTimelines() ? browserSeekMode_ : seekMode_;
    AppendMenuW(menu, MF_STRING | (current == SeekMode::Linked ? MF_CHECKED : 0), CmdSeekLinked,
                L"Linked  (one seek bar moves every video)");
    AppendMenuW(menu, MF_STRING | (current == SeekMode::Independent ? MF_CHECKED : 0), CmdSeekIndependent,
                L"Independent  (each video seeks alone)");
}

// The bar's arrangement button: the Arrangement submenu on its own.
void App::showLayoutMenu(POINT screenPoint) {
    contextPane_ = -1;
    contextMenuOpen_ = true;
    HMENU menu = CreatePopupMenu();
    if (!menu) { contextMenuOpen_ = false; return; }
    appendLayoutMenu(menu);
    const unsigned command = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, window_, nullptr);
    if (command) handleContextCommand(command, -1);
    DestroyMenu(menu);
    contextMenuOpen_ = false;
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
    if (playOrder_ == PlayOrder::RepeatOne) {
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
    if (playOrder_ == PlayOrder::RepeatOne) {
        sourcePaused_[pane] = false;
        seekPaneTo(pane, 0.0, true);
    } else if (playOrder_ != PlayOrder::PlayOne) {
        advanceAtEnd(pane);
    }
    updateControls();
    updateHoverControls();
    updateTitle();
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
    int position = -1;
    const auto files = siblingFiles(paths_[pane], position);
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
