// App: the panes. Opening media into them (the command line's deck, Open
// files and drops, one source into one pane), each pane's offset, volume,
// repeat flag and timing, the layout cells they occupy, pausing one, and
// closing or swapping them. The window, its messages and the per-frame
// tick are App.cpp.

#include "App.hpp"
#include "AppInternal.hpp"
#include "DarkMode.hpp"
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

void App::loadFiles(const std::vector<std::wstring>& files) {
    browserSeekAligned_ = false;
    deviceRecoverySourceDurations_.fill(0.0);
    cancelSeekBarrier();
    clock_.pause();
    // Whatever was playing from a server stops there, at its last position.
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) embyReport(pane, "stopped");
    clock_.seek(0.0);
    if (deviceRecoveryPending_) {
        deviceRecoveryPosition_ = 0.0;
        deviceRecoveryResume_ = false;
    }
    for (auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }
    audio_.flush();
    duration_ = 0.0;
    for (auto& source : sources_) {
        source.reset();
    }
    paths_.fill({});
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) resetPaneMediaState(pane);
    startDelays_.fill(0.0);
    syncAdjustments_.fill(0.0);
    playbackRates_.fill(1.0);
    sourcePaused_.fill(false);
    sourcePausedTimes_.fill(0.0);
    sourceLoopEnabled_.fill(false);
    sourceLoopA_.fill(0.0);
    sourceLoopB_.fill(0.0);
    lastVideoMappedTimes_.fill(-1.0);
    sourceInitialAlignmentPending_.fill(false);
    sourceProvisionalTargets_.fill(0.0);
    masterLoopEnabled_ = false;
    masterLoopA_ = masterLoopB_ = 0.0;
    sessionPath_.clear();

    const std::size_t count = std::min<std::size_t>(kMaxPanes, files.size());
    audioMask_ &= count >= kMaxPanes ? kAllPaneMask : ((1U << count) - 1U);
    if (audioMask_ == 0 && count > 0) audioMask_ = 1U;
    for (std::size_t index = 0; index < count; ++index) {
        openSource(index, files[index]);
    }
    refreshAudioControls();
    updateTitle();
    updateControls();
}

void App::openSource(std::size_t pane, const std::wstring& path, bool resetAdjustment) {
    if (pane >= sources_.size()) return;
    if (resetAdjustment) {
        browserSeekAligned_ = false;
        deviceRecoverySourceDurations_[pane] = 0.0;
    }
    const bool retainEmbyQueue = !resetAdjustment && paths_[pane] == path &&
        emby::isLocator(path) && embyPaneAccountMatches(pane);
    auto retainedQueue = retainEmbyQueue ? std::optional<emby::PlaybackQueue>(embyPanes_[pane]->queue)
                                        : std::nullopt;
    const bool retainedAddedMuted = retainEmbyQueue && embyPanes_[pane]->addedMuted;
    auto retainedLocal = !resetAdjustment && paths_[pane] == path
        ? localPanes_[pane] : std::optional<LocalPanePlayback>{};
    if (perPaneTimelines() || emby::isLocator(path)) detachPaneSeekBarrier(pane);
    const double position = clock_.position();
    const bool playing = clock_.isPlaying();
    embyReport(pane, "stopped");
    if (sources_[pane]) sources_[pane]->setAudioEnabled(false);
    audio_.flush(pane);
    sources_[pane].reset();
    resetPaneMediaState(pane);
    localPanes_[pane] = std::move(retainedLocal);
    if (resetAdjustment) {
        syncAdjustments_[pane] = 0.0;
        sourcePaused_[pane] = false;
        sourcePausedTimes_[pane] = 0.0;
        sourceLoopEnabled_[pane] = false;
        sourceLoopA_[pane] = sourceLoopB_[pane] = 0.0;
    }
    paths_[pane] = path;
    // A failed recovery leaves no stable D3D device for a new D3D11VA source
    // to own. Keep the user's latest path and timing choices, but defer actual
    // decoder construction until recoverLostDevice has completed. This also
    // guarantees that a retry cannot release a device underneath a source
    // opened during the one-second retry interval.
    if (deviceRecoveryPending_ || renderer_.deviceLost()) {
        sourceInitialAlignmentPending_[pane] = false;
        sourceProvisionalTargets_[pane] = 0.0;
        lastVideoMappedTimes_[pane] = -1.0;
        refreshAutoLayoutFocus(paneAspectRatios());
        return;
    }
    if (emby::isLocator(path)) {
        // The stream address comes from the server; the pane keeps its place
        // until the reply arrives and startSourceStream runs.
        embyBeginResolve(pane);
        if (retainedQueue && embyPanes_[pane]) {
            embyPanes_[pane]->queue = std::move(*retainedQueue);
            embyPanes_[pane]->addedMuted = retainedAddedMuted;
        }
        refreshAutoLayoutFocus(paneAspectRatios());
        return;
    }
    // Its subtitles: the files beside it, found off this thread, and the
    // stream inside it that suits the viewer, read from the first packet.
    loadSidecarSubtitles(pane, path);
    startSourceStream(pane, path, localSourceOptions(), -1, position, playing);
    subtitleEmbeddedPending_[pane] = true;
    // The only video brings its folder with it: the list F6 shows and the
    // step keys walk.
    if (singleLoadedPane() == static_cast<int>(pane)) refreshLocalList(path);
}

void App::startSourceStream(std::size_t pane, const std::wstring& url, const SourceOptions& options,
                            int audioStream, double position, bool playing) {
    sources_[pane] = std::make_unique<VideoSource>();
    VideoSource* sourceIdentity = sources_[pane].get();
    sourceIdentity->setAudioOutputPane(pane);
    if (audioStream >= 0) sourceIdentity->setAudioTrack(audioStream);
    sources_[pane]->open(
        url, renderer_.device(), renderer_.deviceContext(),
        &renderer_.deviceMutex(), decodeMode_, [this, sourceIdentity](AudioChunk&& chunk) {
            return audio_.submit(sourceIdentity->audioOutputPane(), std::move(chunk));
        }, nasCacheEnabled_ ? CacheMode::Network : CacheMode::Off, options);
    sources_[pane]->setAudioEnabled(false);
    const double provisionalTarget = mappedSourceTime(pane, position);
    sourceInitialAlignmentPending_[pane] = true;
    sourceProvisionalTargets_[pane] = provisionalTarget;
    lastVideoMappedTimes_[pane] = provisionalTarget;
    sources_[pane]->requestSeek(provisionalTarget);
    const auto audioStatus = sources_[pane]->audioDecodeStatus();
    audio_.flush(pane, audioStatus.generation);
    const bool useAudio = shouldOutputAudio(pane, position, playing);
    sources_[pane]->setAudioEnabled(useAudio);
    if (useAudio) {
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
        audio_.play(pane);
    }
    refreshAutoLayoutFocus(paneAspectRatios());
}

void App::addFiles(const std::vector<std::wstring>& files, int targetPane) {
    if (files.empty()) return;
    // One file dropped on the only video replaces it, and the deck is again
    // one video, which starts from zero; anything else joins the deck's time.
    const bool replacesOnlyVideo = files.size() == 1 && targetPane >= 0 && singleLoadedPane() == targetPane;
    const bool resume = playbackIntended();
    const bool resumeInterruptedBarrier = seekBarrier_.active() && seekBarrier_.resume();
    const double interruptedPosition = clock_.position();
    if (seekBarrier_.active()) cancelSeekBarrier();
    bool hasSelectedLoadedAudio = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        hasSelectedLoadedAudio = hasSelectedLoadedAudio ||
            (paneLogicallyLoaded(pane) && audioPaneEnabled(pane));
    }
    const bool chooseNewAudioPane = !hasSelectedLoadedAudio;
    std::vector<std::size_t> destinations;
    PaneArray<bool> reserved{};

    if (targetPane >= 0 && targetPane < static_cast<int>(kMaxPanes)) {
        destinations.push_back(static_cast<std::size_t>(targetPane));
        reserved[static_cast<std::size_t>(targetPane)] = true;
    }
    for (std::size_t pane = 0; pane < sources_.size() && destinations.size() < files.size(); ++pane) {
        if (!paneLogicallyLoaded(pane) && !reserved[pane]) {
            destinations.push_back(pane);
            reserved[pane] = true;
        }
    }
    const std::size_t count = std::min(files.size(), destinations.size());
    for (std::size_t index = 0; index < count; ++index) {
        openSource(destinations[index], files[index]);
    }
    const auto activeAfterAdd = activePanes();
    expandedPane_ = retainedExpandedPane(expandedPane_, activeAfterAdd);
    if (chooseNewAudioPane && count > 0) {
        setAudioMask(1U << destinations[0]);
    }
    if (count == 0) {
        MessageBoxW(window_, L"All five slots are occupied. Drag a file onto a video pane to replace it.",
                    L"QuadDeck", MB_OK | MB_ICONINFORMATION);
    } else if (count < files.size()) {
        MessageBoxW(window_,
            L"Only five video slots are available. To replace a full slot, drag one file directly onto that video pane.",
            L"QuadDeck", MB_OK | MB_ICONINFORMATION);
    }
    updateTitle();
    updateControls();
    refreshAudioControls();
    if (replacesOnlyVideo) {
        restartReplacedOnlyVideo(resume);
    } else if (resumeInterruptedBarrier) {
        beginSynchronizedSeek(interruptedPosition, false, true);
    }
}

// Collapse the legacy delay-plus-correction pair into one directly editable
// signed offset, so a source waiting for its start can also be moved forward.
// Writes one pane's offset without re-aligning anything, so a batch change can
// pay for a single synchronized seek instead of one per pane.
void App::applyPaneOffset(std::size_t pane, double seconds) {
    if (pane >= sources_.size()) return;
    startDelays_[pane] = 0.0;
    syncAdjustments_[pane] = seconds;
    if (sourcePaused_[pane]) {
        // A paused pane shows its held time, and resuming rebuilds the
        // adjustment from that held value. Leaving it behind would restore the
        // offset that was just changed the moment the pane resumes.
        sourcePausedTimes_[pane] =
            sourceTime(clock_.position(), 0.0, seconds, playbackRates_[pane]);
    }
}

void App::setPaneOffset(std::size_t pane, double seconds) {
    if (pane >= sources_.size() || !std::isfinite(seconds)) return;
    browserSeekAligned_ = false;
    applyPaneOffset(pane, std::clamp(seconds, -86400.0, 86400.0));
    seekAbsolute(clock_.position());
}

// The signed value the UI calls Offset, which folds the legacy start delay into
// the adjustment so that sessions written before the two merged still read back
// as one number.
double App::paneOffsetSeconds(std::size_t pane) const {
    if (pane >= sources_.size()) return 0.0;
    return effectiveTimelineOffset(
        startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]);
}

bool App::anyPaneOffset() const {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (sources_[pane] && paneOffsetSeconds(pane) != 0.0) return true;
    }
    return false;
}

void App::resetPaneOffset(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    browserSeekAligned_ = false;
    applyPaneOffset(pane, 0.0);
    seekAbsolute(clock_.position());
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) + L" offset reset");
    updateControls();
    updateTitle();
}

// One synchronized seek for the whole batch: five separate resets would each
// freeze the clock and wait for every pane to land, which is the slowest thing
// this player does.
void App::resetAllOffsets() {
    bool changed = false;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane] || paneOffsetSeconds(pane) == 0.0) continue;
        applyPaneOffset(pane, 0.0);
        changed = true;
    }
    if (!changed) return;
    browserSeekAligned_ = false;
    seekAbsolute(clock_.position());
    showNotice(L"All offsets reset");
    updateControls();
    updateTitle();
}

void App::setPaneVolume(std::size_t pane, float volume) {
    if (pane >= sources_.size() || !std::isfinite(volume)) return;
    audio_.setPaneVolume(pane, volume);
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) + L" volume " +
               std::to_wstring(static_cast<int>(std::lround(audio_.paneVolume(pane) * 100.0F))) +
               L"%");
    scheduleAppSettingsSave();
}

void App::nudgePaneVolume(std::size_t pane, float delta) {
    if (pane >= sources_.size()) return;
    setPaneVolume(pane, audio_.paneVolume(pane) + delta);
}

void App::togglePaneMute(std::size_t pane) {
    if (pane >= sources_.size()) return;
    audio_.setPaneMuted(pane, !audio_.paneMuted(pane));
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) +
               (audio_.paneMuted(pane) ? L" muted" : L" unmuted"));
    scheduleAppSettingsSave();
}

void App::setSourceAutoRepeat(std::size_t pane, bool enabled) {
    if (pane >= sources_.size()) return;
    if (sourceAutoRepeat_[pane] == enabled &&
        !(localPanes_[pane] && localPanes_[pane]->suppressInheritedRepeat)) return;
    // Treat the toggle as one instant. Sampling the running clock twice can
    // manufacture a mapping delta larger than the comparison tolerance and
    // flush a long-GOP decoder even when repeat does not change this cycle.
    const double timeline = clock_.position();
    const double before = mappedSourceTime(pane, timeline);
    if (localPanes_[pane]) localPanes_[pane]->suppressInheritedRepeat = false;
    sourceAutoRepeat_[pane] = enabled;
    scheduleAppSettingsSave();
    // Linked mode never changes its mapping for per-pane repeat, and the first
    // Independent cycle does not change either. Do not flush a decoder and
    // blank a long-GOP pane when this setting is only changing future policy.
    realignPaneAfterMappingChange(pane, timeline, before);
}

void App::realignPaneAfterMappingChange(
    std::size_t pane, double timeline, double previousMappedTime) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    const double mapped = mappedSourceTime(pane, timeline);
    if (!timelineMappingChanged(previousMappedTime, mapped)) return;

    if (seekBarrier_.active() && !perPaneTimelines()) {
        seekAbsolute(timeline);
        return;
    }
    detachPaneSeekBarrier(pane);

    // Stop the producer before allocating the replacement seek generation.
    // The explicit flush then rejects every older in-flight chunk without
    // accidentally rejecting audio decoded for this new generation.
    sources_[pane]->setAudioEnabled(false);
    lastVideoMappedTimes_[pane] = mapped;
    if (sourceInitialAlignmentPending_[pane]) {
        sourceProvisionalTargets_[pane] = mapped;
    }
    sources_[pane]->requestSeek(mapped);
    const auto audioStatus = sources_[pane]->audioDecodeStatus();
    audio_.flush(pane, audioStatus.generation);
    const bool useAudio = shouldOutputAudio(pane, timeline, clock_.isPlaying());
    sources_[pane]->setAudioEnabled(useAudio);
    if (useAudio) audio_.play(pane);
}

// Move one source along its own timeline, leaving the master clock and every
// other pane where they are. The shift is expressed as this pane's offset, so
// it survives as an ordinary synchronisation correction rather than as hidden
// state.
void App::seekPaneTo(std::size_t pane, double target, bool forceIndependent) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    browserSeekAligned_ = false;
    const double duration = sources_[pane]->duration();
    target = std::max(0.0, target);
    if (duration > 0.0) target = std::min(target, duration);

    const double timelinePosition = clock_.position();
    const double naturalPosition = (timelinePosition - startDelays_[pane]) * playbackRates_[pane];
    syncAdjustments_[pane] = target - naturalPosition;
    if (sourcePaused_[pane]) sourcePausedTimes_[pane] = target;
    if (embyPanes_[pane]) embyPanes_[pane]->endHandled = false;
    if (localPanes_[pane]) localPanes_[pane]->endHandled = false;
    if (seekBarrier_.active() && !perPaneTimelines() && !forceIndependent) {
        seekAbsolute(timelinePosition);
        updateTitle();
        return;
    }
    detachPaneSeekBarrier(pane);
    sources_[pane]->setAudioEnabled(false);
    // Show the nearest keyframe at once rather than leaving the pane on its
    // old frame until the exact one lands.
    lastVideoMappedTimes_[pane] = target;
    if (sourceInitialAlignmentPending_[pane]) {
        sourceProvisionalTargets_[pane] = target;
    }
    sources_[pane]->requestSeek(target, true);
    const auto audioStatus = sources_[pane]->audioDecodeStatus();
    audio_.flush(pane, audioStatus.generation);
    const bool useAudio = shouldOutputAudio(pane, timelinePosition, clock_.isPlaying());
    sources_[pane]->setAudioEnabled(useAudio);
    if (useAudio) {
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
        audio_.play(pane);
    }
    updateControls();
    updateTitle();
}

PaneTiming App::paneTiming(std::size_t pane) const {
    PaneTiming timing;
    if (pane >= sources_.size()) return timing;
    timing.paused = sourcePaused_[pane];
    timing.pausedTime = sourcePausedTimes_[pane];
    timing.startDelay = startDelays_[pane];
    timing.adjustment = syncAdjustments_[pane];
    timing.rate = playbackRates_[pane];
    timing.loopEnabled = sourceLoopEnabled_[pane];
    timing.loopA = sourceLoopA_[pane];
    timing.loopB = sourceLoopB_[pane];
    timing.autoRepeat = sourceAutoRepeat_[pane];
    // Adopting a lone Explorer source must not activate a repeat preference
    // that its previous Linked mapping ignored. An explicit repeat choice
    // or the next opened item ends this transition-only suppression.
    if (localPanes_[pane] && localPanes_[pane]->suppressInheritedRepeat) timing.autoRepeat = false;
    // Browser playback needs independent timelines, but manually opened locals
    // retain their previous seek policy: one is Linked, several use the
    // saved mode. Adding a pane must not suddenly wrap an ended local source.
    if (!paneRepeatFlagInForce(paneOrigins(), pane, seekMode_)) timing.autoRepeat = false;
    if (sources_[pane]) {
        timing.loaded = true;
        timing.ready = sources_[pane]->ready();
        timing.duration = sources_[pane]->duration();
    }
    return timing;
}

PaneTimings App::paneTimings() const {
    PaneTimings timings{};
    for (std::size_t pane = 0; pane < timings.size(); ++pane) {
        timings[pane] = paneTiming(pane);
    }
    return timings;
}

double App::mappedSourceTime(std::size_t pane, double timeline) const {
    if (pane >= sources_.size()) return 0.0;
    return mappedPaneTime(paneTiming(pane), timeline, activeSeekMode());
}

double App::currentSourceTime(std::size_t pane) const {
    double value = mappedSourceTime(pane, clock_.position());
    if (pane < sources_.size() && sources_[pane] && sources_[pane]->duration() > 0.0) {
        value = std::clamp(value, 0.0, sources_[pane]->duration());
    }
    return value;
}

PaneArray<bool> App::activePanes() const {
    PaneArray<bool> active{};
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        active[index] = paneLogicallyLoaded(index);
    }
    return active;
}

bool App::paneLogicallyLoaded(std::size_t pane) const {
    if (pane >= sources_.size()) return false;
    return sources_[pane] != nullptr ||
           ((deviceRecoveryPending_ || renderer_.deviceLost()) &&
            !paths_[pane].empty());
}

PaneArray<float> App::paneAspectRatios() const {
    PaneArray<float> aspects{};
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane]) continue;
        const int width = sources_[pane]->videoWidth();
        const int height = sources_[pane]->videoHeight();
        if (width > 0 && height > 0) {
            aspects[pane] = static_cast<float>(width) / static_cast<float>(height);
        }
    }
    return aspects;
}

void App::refreshAutoLayoutFocus(const PaneArray<float>& aspects) {
    const auto active = activePanes();
    bool changed = false;
    if (active != autoLayoutActivePanes_) {
        autoLayoutActivePanes_ = active;
        autoLayoutFocus_ = AutoLayoutFocus::Dynamic;
        autoLayoutFocusPane_ = -1;
        changed = true;
    }
    if (autoLayoutFocus_ == AutoLayoutFocus::Dynamic) {
        int activeCount = 0;
        int portraitCount = 0;
        int landscapeCount = 0;
        int portraitPane = -1;
        int landscapePane = -1;
        bool allAspectsKnown = true;
        for (std::size_t pane = 0; pane < active.size(); ++pane) {
            if (!active[pane]) continue;
            ++activeCount;
            if (aspects[pane] <= 0.0F) {
                allAspectsKnown = false;
                continue;
            }
            if (aspects[pane] < 0.90F) {
                ++portraitCount;
                portraitPane = static_cast<int>(pane);
            } else {
                ++landscapeCount;
                landscapePane = static_cast<int>(pane);
            }
        }
        if (activeCount < 3) {
            autoLayoutFocus_ = AutoLayoutFocus::None;
            changed = true;
        } else if (allAspectsKnown) {
            if (portraitCount == 1) {
                autoLayoutFocus_ = AutoLayoutFocus::Tall;
                autoLayoutFocusPane_ = portraitPane;
            } else if (landscapeCount == 1) {
                autoLayoutFocus_ = AutoLayoutFocus::Wide;
                autoLayoutFocusPane_ = landscapePane;
            } else {
                autoLayoutFocus_ = AutoLayoutFocus::None;
            }
            changed = true;
        }
    }
    if (changed) {
        // Force all geometry consumers to refresh even
        // when an added pane has not published its aspect ratio yet.
        lastLayoutAspects_.fill(-1.0F);
    }
}

PaneArray<RectF> App::currentLayoutCells(
    float width, float height, const PaneArray<float>& aspects) const {
    return activeLayoutCells(width, height, activePanes(), layoutMode_, expandedPane_,
                             soloPane_, aspects, autoLayoutFocus_, autoLayoutFocusPane_);
}

PaneArray<int> App::panesByPosition() const {
    // Numbering describes the normal multi-pane layout. Entering Solo should
    // not rename the selected video to V1 until Solo is left again.
    const auto cells = activeLayoutCells(
        1000.0F, 1000.0F, activePanes(), layoutMode_, expandedPane_, -1,
        paneAspectRatios(), autoLayoutFocus_, autoLayoutFocusPane_);
    return panesByDisplayPosition(cells);
}

std::size_t App::paneForPosition(std::size_t position) const {
    const auto order = panesByPosition();
    if (position >= order.size() || order[position] < 0) return 0;
    return static_cast<std::size_t>(order[position]);
}

std::size_t App::positionForPane(std::size_t pane) const {
    const auto order = panesByPosition();
    const auto found = std::find(order.begin(), order.end(), static_cast<int>(pane));
    return found == order.end() ? std::min(pane, order.size() - 1)
                                : static_cast<std::size_t>(found - order.begin());
}

void App::toggleSourcePause(std::size_t pane) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    const double timeline = clock_.position();
    const bool restartBarrier = seekBarrier_.active() && !perPaneTimelines();
    if (!restartBarrier) detachPaneSeekBarrier(pane);
    if (!sourcePaused_[pane]) {
        sourcePausedTimes_[pane] = mappedSourceTime(pane, timeline);
        sourcePaused_[pane] = true;
        sources_[pane]->setAudioEnabled(false);
        if (audioPaneEnabled(pane)) audio_.flush(pane);
    } else {
        const double held = sourcePausedTimes_[pane];
        const double natural = (timeline - startDelays_[pane]) * playbackRates_[pane];
        syncAdjustments_[pane] = held - natural;
        sourcePaused_[pane] = false;
        if (embyPanes_[pane]) embyPanes_[pane]->endHandled = false;
        if (localPanes_[pane]) localPanes_[pane]->endHandled = false;
        // Paused mapping deliberately returns the held frame without applying
        // A-B or whole-source repeat. Those policies may have changed while
        // the pane was paused, so calculate the real resumed destination only
        // after clearing the paused flag.
        const double resumedMapped = mappedSourceTime(pane, timeline);
        lastVideoMappedTimes_[pane] = resumedMapped;
        if (!restartBarrier) {
            if (sourceInitialAlignmentPending_[pane]) {
                sourceProvisionalTargets_[pane] = resumedMapped;
            }
            sources_[pane]->requestSeek(resumedMapped);
            const auto audioStatus = sources_[pane]->audioDecodeStatus();
            audio_.flush(pane, audioStatus.generation);
            const bool useAudio = shouldOutputAudio(pane, timeline, clock_.isPlaying());
            sources_[pane]->setAudioEnabled(useAudio);
            if (useAudio) {
                audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
                audio_.play(pane);
            }
        }
    }
    if (restartBarrier) seekAbsolute(timeline);
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) +
               (sourcePaused_[pane] ? L" paused" : L" resumed"));
    updateHoverControls();
    updateTitle();
}

void App::closePane(std::size_t pane) {
    if (pane >= sources_.size()) return;
    browserSeekAligned_ = false;
    deviceRecoverySourceDurations_[pane] = 0.0;
    const bool wasEmbyDeck = perPaneTimelines();
    if (wasEmbyDeck) detachPaneSeekBarrier(pane);
    embyReport(pane, "stopped");
    if (sources_[pane]) sources_[pane]->setAudioEnabled(false);
    audio_.flush(pane);
    sources_[pane].reset();
    paths_[pane].clear();
    resetPaneMediaState(pane);
    startDelays_[pane] = syncAdjustments_[pane] = sourcePausedTimes_[pane] = 0.0;
    playbackRates_[pane] = 1.0;
    sourcePaused_[pane] = sourceLoopEnabled_[pane] = false;
    sourceLoopA_[pane] = sourceLoopB_[pane] = 0.0;
    lastVideoMappedTimes_[pane] = -1.0;
    sourceInitialAlignmentPending_[pane] = false;
    sourceProvisionalTargets_[pane] = 0.0;
    if (soloPane_ == static_cast<int>(pane)) soloPane_ = -1;
    const auto activeAfterClose = activePanes();
    expandedPane_ = retainedExpandedPane(expandedPane_, activeAfterClose);
    unsigned newMask = audioMask_ & ~(1U << pane);
    if (newMask == 0 && !wasEmbyDeck) {
        for (std::size_t index = 0; index < sources_.size(); ++index) {
            if (paneLogicallyLoaded(index)) { newMask = 1U << index; break; }
        }
    }
    setAudioMask(newMask);
    refreshAutoLayoutFocus(paneAspectRatios());
    updateControls();
    updateHoverControls();
    updateTitle();
}

void App::swapPanes(std::size_t first, std::size_t second) {
    if (first >= sources_.size() || second >= sources_.size() || first == second ||
        !sources_[first] || !sources_[second]) return;
    const bool independentDeck = perPaneTimelines();
    if (independentDeck) {
        detachPaneSeekBarrier(first);
        detachPaneSeekBarrier(second);
    }
    // A VideoSource's audio callback used to capture the pane voice it was
    // opened for. Moving the unique_ptr then left the picture in its new cell
    // while audio kept going to the old voice. Stop both fixed voices,
    // retarget each callback atomically, then move the media state.
    if (sources_[first]) sources_[first]->setAudioEnabled(false);
    if (sources_[second]) sources_[second]->setAudioEnabled(false);
    audio_.flush(first);
    audio_.flush(second);
    sources_[first]->setAudioOutputPane(second);
    sources_[second]->setAudioOutputPane(first);
    std::swap(sources_[first], sources_[second]);
    std::swap(paths_[first], paths_[second]);
    std::swap(startDelays_[first], startDelays_[second]);
    std::swap(syncAdjustments_[first], syncAdjustments_[second]);
    std::swap(playbackRates_[first], playbackRates_[second]);
    std::swap(sourcePaused_[first], sourcePaused_[second]);
    std::swap(sourcePausedTimes_[first], sourcePausedTimes_[second]);
    std::swap(sourceLoopEnabled_[first], sourceLoopEnabled_[second]);
    std::swap(sourceLoopA_[first], sourceLoopA_[second]);
    std::swap(sourceLoopB_[first], sourceLoopB_[second]);
    std::swap(sourceAutoRepeat_[first], sourceAutoRepeat_[second]);
    std::swap(lastVideoMappedTimes_[first], lastVideoMappedTimes_[second]);
    std::swap(sourceInitialAlignmentPending_[first], sourceInitialAlignmentPending_[second]);
    std::swap(sourceProvisionalTargets_[first], sourceProvisionalTargets_[second]);
    std::swap(paneViews_[first], paneViews_[second]);
    // The media's own state travels with it; a reply still in flight for
    // either pane finds it again by serial.
    std::swap(embyPanes_[first], embyPanes_[second]);
    std::swap(localPanes_[first], localPanes_[second]);
    if (embyTargetPane_ == static_cast<int>(first)) embyTargetPane_ = static_cast<int>(second);
    else if (embyTargetPane_ == static_cast<int>(second)) embyTargetPane_ = static_cast<int>(first);
    std::swap(subtitles_[first], subtitles_[second]);
    std::swap(subtitleSelection_[first], subtitleSelection_[second]);
    std::swap(subtitleFiles_[first], subtitleFiles_[second]);
    std::swap(subtitleFilesWithoutLines_[first], subtitleFilesWithoutLines_[second]);
    std::swap(subtitleCycleAnchor_[first], subtitleCycleAnchor_[second]);
    std::swap(subtitleSerial_[first], subtitleSerial_[second]);
    std::swap(subtitleChosenByViewer_[first], subtitleChosenByViewer_[second]);
    std::swap(subtitleEmbeddedPending_[first], subtitleEmbeddedPending_[second]);
    std::swap(subtitleDelay_[first], subtitleDelay_[second]);
    std::swap(assPanes_[first], assPanes_[second]);
    audio_.swapPaneSettings(first, second);
    const bool firstAudio = audioPaneEnabled(first);
    const bool secondAudio = audioPaneEnabled(second);
    if (firstAudio != secondAudio) audioMask_ ^= (1U << first) | (1U << second);
    if (soloPane_ == static_cast<int>(first)) soloPane_ = static_cast<int>(second);
    else if (soloPane_ == static_cast<int>(second)) soloPane_ = static_cast<int>(first);
    if (expandedPane_ == static_cast<int>(first)) expandedPane_ = static_cast<int>(second);
    else if (expandedPane_ == static_cast<int>(second)) expandedPane_ = static_cast<int>(first);
    refreshAudioControls();
    if (independentDeck) {
        const double timeline = clock_.position();
        for (const auto pane : {first, second}) {
            const double mapped = mappedSourceTime(pane, timeline);
            const auto generation = sources_[pane]->requestAudioSeek(mapped);
            audio_.flush(pane, generation);
            const bool useAudio = shouldOutputAudio(pane, timeline, clock_.isPlaying());
            sources_[pane]->setAudioEnabled(useAudio);
            audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
            if (useAudio) audio_.play(pane);
        }
    } else {
        seekAbsolute(clock_.position());
    }
    layoutHoverControls();
}

}  // namespace quaddeck
