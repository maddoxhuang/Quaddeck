// App: transport, seeking and the synchronized seek barrier, and audio
// selection including automatic handoff between panes.

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

void App::togglePlayback() {
    if (deviceRecoveryPending_) {
        // Decoder sources do not exist between device-recovery attempts. Keep
        // transport input meaningful by changing the intent that will be
        // restored, rather than starting an empty seek barrier which the next
        // retry would silently overwrite.
        deviceRecoveryResume_ = !deviceRecoveryResume_;
        updateTitle();
        updateControls();
        return;
    }
    if (seekBarrier_.active()) {
        const bool resume = !seekBarrier_.resume();
        seekBarrier_.setResume(resume);
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (!sources_[pane]) continue;
            if (!resume) {
                if (!seekBarrier_.audioWaiting(pane)) continue;
                sources_[pane]->setAudioEnabled(false);
                audio_.flush(pane);
                seekBarrier_.setAudioWaiting(pane, false);
                continue;
            }
            if (!seekBarrier_.waiting(pane) || !audioPaneEnabled(pane)) continue;
            sources_[pane]->setAudioEnabled(false);
            const auto audioGeneration = sources_[pane]->requestAudioSeek(
                seekBarrier_.paneTarget(pane));
            audio_.flush(pane, audioGeneration);
            seekBarrier_.setAudioGeneration(pane, audioGeneration);
            seekBarrier_.setAudioWaiting(pane, true);
            sources_[pane]->setAudioEnabled(true);
        }
        updateTitle();
        updateControls();
        return;
    }
    if (clock_.isPlaying()) {
        clock_.pause();
        for (auto& source : sources_) {
            if (source) source->setAudioEnabled(false);
        }
        audio_.pause();
        audio_.flush();
    } else {
        beginSynchronizedSeek(clock_.position(), false, true);
    }
    showNotice(playbackIntended() ? L"Playing" : L"Paused");
    updateTitle();
    updateControls();
}

void App::stopPlayback() {
    const bool independentDeck = perPaneTimelines();
    if (independentDeck) {
        const double timeline = clock_.position();
        for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
            if (paths_[pane].empty()) continue;
            if (!sources_[pane]) {
                syncAdjustments_[pane] = -(timeline - startDelays_[pane]) * playbackRates_[pane];
                sourcePausedTimes_[pane] = 0.0;
            }
            if (embyPanes_[pane]) {
                // Stop also wins over a resume position arriving from a
                // request that was already in flight.
                embyPanes_[pane]->fromBeginning = true;
                embyPanes_[pane]->resumeTicks = 0;
                embyPanes_[pane]->endHandled = false;
            }
            if (localPanes_[pane]) localPanes_[pane]->endHandled = false;
        }
    }
    if (deviceRecoveryPending_) {
        deviceRecoveryResume_ = false;
        seekAbsolute(independentDeck ? clock_.position() : 0.0);
        updateControls();
        return;
    }
    cancelSeekBarrier();
    clock_.pause();
    for (auto& source : sources_) if (source) source->setAudioEnabled(false);
    audio_.pause();
    audio_.flush();
    if (independentDeck) {
        // Every source returns to its own zero under a parked master clock.
        // Moving master time to zero alone would restore each resume offset.
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (sources_[pane]) seekPaneTo(pane, 0.0);
        }
    } else {
        seekAbsolute(0.0);
    }
    showNotice(L"Stopped");
    updateControls();
}

void App::seekRelative(double delta) {
    pendingSeekDirection_ = delta > 0.0 ? 1 : delta < 0.0 ? -1 : 0;
    if (linkedBrowserSeekBars()) {
        const auto shown = barTime();
        alignBrowserPanesToTime(std::clamp(shown.position, 0.0,
            std::max(shown.duration, 0.0)) + delta);
        return;
    }
    if (deviceRecoveryPending_) {
        seekAbsolute(deviceRecoveryPosition_ + delta, SeekStyle::Fast);
        return;
    }
    // Independent seek bars mean the panes are deliberately not on one
    // timeline, so an arrow key moving every pane would undo exactly the
    // separation the mode exists to provide. Move the pane being looked at
    // instead, chosen the same way the file-stepping commands choose one.
    if (activeSeekMode() == SeekMode::Independent) {
        const int pane = commandPane();
        if (pane >= 0) {
            const auto index = static_cast<std::size_t>(pane);
            seekPaneTo(index, currentSourceTime(index) + delta);
            showNotice(L"V" + std::to_wstring(positionForPane(index) + 1) + L"  " +
                       formatTime(currentSourceTime(index)));
            return;
        }
    }
    const double target = std::clamp(clock_.position() + delta, 0.0, std::max(0.0, duration_));
    seekAbsolute(target, SeekStyle::Fast);
    showNotice(formatTime(target) + L" / " + formatTime(std::max(0.0, duration_)));
}

void App::seekAbsolute(double position, SeekStyle style, bool keepEmbyPhotos) {
    const int direction = pendingSeekDirection_;
    pendingSeekDirection_ = 0;
    position = std::max(0.0, position);
    if (duration_ > 0.0) {
        position = std::min(position, duration_);
    }
    if (deviceRecoveryPending_) {
        // Keep the paused clock in sync for labels and sliders, while saving
        // the destination that will be issued once a valid device exists.
        cancelSeekBarrier();
        clock_.pause();
        clock_.setSlew(1.0);
        clock_.seek(position);
        deviceRecoveryPosition_ = position;
        updateTitle();
        updateControls();
        return;
    }
    const bool playing = clock_.isPlaying() || (seekBarrier_.active() && seekBarrier_.resume());
    if (style == SeekStyle::Synchronized && playing) {
        beginSynchronizedSeek(position, false, true);
        updateTitle();
        updateControls();
        return;
    }
    // Either nothing is playing, or the caller wants the timeline to respond
    // now. Both take the same path: move the clock, ask every source for the
    // new position, and let each pane catch up on its own. No barrier means
    // nothing waits for the slowest decoder.
    cancelSeekBarrier();
    clock_.setSlew(1.0);
    clock_.seek(position);
    if (playing && !clock_.isPlaying()) {
        // A barrier that was going to resume had parked the clock.
        clock_.play();
    }
    for (auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }
    const auto origins = keepEmbyPhotos ? paneOrigins() : PaneOrigins{};
    for (std::size_t index = 0; index < sources_.size(); ++index) {
        if (!sources_[index]) {
            audio_.flush(index);
            continue;
        }
        const double mapped = mappedSourceTime(index, position);
        lastVideoMappedTimes_[index] = mapped;
        if (keepEmbyPhotos && origins[index] == PaneOrigin::EmbyPhoto) {
            // The still already has its frame. A seek would clear it, while
            // moving the other videos must leave this pane untouched.
            audio_.flush(index);
            continue;
        }
        if (sourceInitialAlignmentPending_[index]) {
            sourceProvisionalTargets_[index] = mapped;
        }
        // A fast seek publishes the nearest keyframe as an immediate still, so
        // the pane shows roughly the right place while the exact frame is
        // still being decoded. Dependency frames in between stay hidden, so
        // this never becomes a visible fast-forward.
        // The only video lands on a keyframe: instant, as PotPlayer does.
        // A set of videos seeks exactly, or they would not stay in step.
        if (style == SeekStyle::Fast && keyframeSeek_ && singleLoadedPane() == static_cast<int>(index)) {
            sources_[index]->requestKeyframeSeek(mapped, direction);
        } else {
            sources_[index]->requestSeek(mapped, style == SeekStyle::Fast);
        }
        const auto audioStatus = sources_[index]->audioDecodeStatus();
        audio_.flush(index, audioStatus.generation);
        const bool useAudio = shouldOutputAudio(index, position, playing);
        sources_[index]->setAudioEnabled(useAudio);
        audio_.setRate(index, static_cast<float>(playbackRates_[index]));
        if (useAudio) audio_.play(index);
    }
    updateTitle();
    updateControls();
}

// Explicit browser-linked seeks align source-local seconds. Browser queues,
// repeat and EOF still use their independent timeline policy. Rebase all
// offsets against the same new master-clock sample before issuing one fast
// seek, so a slow network pane does not hold the other pictures.
void App::alignBrowserPanesToTime(double requested) {
    if (!linkedBrowserSeekBars() || !std::isfinite(requested)) return;
    const auto origins = paneOrigins();
    if (deviceRecoveryPending_) {
        // Recovery retains paths and adjustments while decoders are absent.
        // Save the same source-second target in those adjustments; openSource
        // will reuse them when the device and streams return.
        double longest = 0.0;
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
            const double knownDuration = sources_[pane] ? sources_[pane]->duration()
                                                         : deviceRecoverySourceDurations_[pane];
            if (knownDuration <= 0.0) {
                pendingSeekDirection_ = 0;
                showNotice(L"Wait for video lengths after device recovery");
                return;
            }
            longest = std::max(longest, knownDuration);
        }
        if (longest <= 0.0) {
            pendingSeekDirection_ = 0;
            return;
        }
        const double target = std::clamp(requested, 0.0, longest);
        duration_ = target;
        for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
            if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
            const double knownDuration = sources_[pane] ? sources_[pane]->duration()
                                                         : deviceRecoverySourceDurations_[pane];
            const double paneTarget = std::min(target, knownDuration);
            syncAdjustments_[pane] = paneTarget -
                (target - startDelays_[pane]) * playbackRates_[pane];
            const bool heldAtEnd = (embyPanes_[pane] && embyPanes_[pane]->endHandled) ||
                                   (localPanes_[pane] && localPanes_[pane]->endHandled);
            if (heldAtEnd) sourcePaused_[pane] = false;
            if (sourcePaused_[pane]) sourcePausedTimes_[pane] = paneTarget;
            if (embyPanes_[pane]) embyPanes_[pane]->endHandled = false;
            if (localPanes_[pane]) localPanes_[pane]->endHandled = false;
            duration_ = std::max(duration_, timelineDuration(knownDuration,
                startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]));
        }
        browserSeekAligned_ = true;
        seekAbsolute(target, SeekStyle::Fast, true);
        showNotice(L"Aligned video time to " + formatTime(target));
        return;
    }
    double longest = 0.0;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
        if (sources_[pane] && !sources_[pane]->error().empty()) {
            pendingSeekDirection_ = 0;
            showNotice(L"Close the failed video before aligning");
            return;
        }
        if (!sources_[pane] || !sources_[pane]->ready()) {
            pendingSeekDirection_ = 0;
            showNotice(L"Wait for every video to finish opening before aligning");
            return;
        }
        if (sources_[pane]->duration() <= 0.0) {
            pendingSeekDirection_ = 0;
            showNotice(L"Cannot align a video with unknown duration");
            return;
        }
        longest = std::max(longest, sources_[pane]->duration());
    }
    if (longest <= 0.0) {
        pendingSeekDirection_ = 0;
        return;
    }
    const double target = std::clamp(requested, 0.0, longest);
    duration_ = target;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (origins[pane] == PaneOrigin::Empty || origins[pane] == PaneOrigin::EmbyPhoto) continue;
        // A shorter video can only reach its own end. Its repeat/loop rule
        // remains in force and may remap that end during this seek.
        const double paneTarget = std::min(target, sources_[pane]->duration());
        syncAdjustments_[pane] = paneTarget -
            (target - startDelays_[pane]) * playbackRates_[pane];
        const bool heldAtEnd = (embyPanes_[pane] && embyPanes_[pane]->endHandled) ||
                               (localPanes_[pane] && localPanes_[pane]->endHandled);
        if (heldAtEnd) sourcePaused_[pane] = false;
        if (sourcePaused_[pane]) sourcePausedTimes_[pane] = paneTarget;
        if (embyPanes_[pane]) embyPanes_[pane]->endHandled = false;
        if (localPanes_[pane]) localPanes_[pane]->endHandled = false;
        duration_ = std::max(duration_, timelineDuration(
            sources_[pane]->duration(), startDelays_[pane], syncAdjustments_[pane], playbackRates_[pane]));
    }
    browserSeekAligned_ = true;
    seekAbsolute(target, SeekStyle::Fast, true);
    showNotice(L"Aligned video time to " + formatTime(target));
}

void App::beginSynchronizedSeek(double position, bool showKeyframePreview, bool resume) {
    cancelSeekBarrier();
    seekBarrierStarted_ = std::chrono::steady_clock::now();
    // The tracking error says nothing across a discontinuity; start over.
    clock_.setSlew(1.0);
    clock_.pause();
    clock_.seek(position);
    audio_.pause();
    for (auto& source : sources_) {
        if (source) source->setAudioEnabled(false);
    }

    const bool anyWait = seekBarrier_.begin(
        position, resume, paneTimings(), audioMask_, activeSeekMode());
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane]) {
            audio_.flush(pane);
            continue;
        }
        const double mapped = seekBarrier_.paneTarget(pane);
        lastVideoMappedTimes_[pane] = mapped;
        if (sourceInitialAlignmentPending_[pane]) {
            sourceProvisionalTargets_[pane] = mapped;
        }
        const auto videoGeneration =
            sources_[pane]->requestSeek(mapped, showKeyframePreview);
        const auto audioStatus = sources_[pane]->audioDecodeStatus();
        audio_.flush(pane, audioStatus.generation);
        seekBarrier_.setGeneration(pane, videoGeneration);
        seekBarrier_.setAudioGeneration(pane, audioStatus.generation);
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
        sources_[pane]->setAudioEnabled(seekBarrier_.audioWaiting(pane));
    }
    std::ostringstream message;
    message << "Seek barrier target=" << std::fixed << std::setprecision(3) << position
            << " resume=" << resume << " preview=" << showKeyframePreview;
    appendDiagnostic(message.str());
    if (!anyWait) completeSeekBarrier();
}

void App::cancelSeekBarrier() {
    seekBarrier_.reset();
}

void App::detachPaneSeekBarrier(std::size_t pane) {
    if (pane >= sources_.size() || !seekBarrier_.active()) return;
    // A per-pane replacement, seek or pause retires only that decoder's
    // ticket. The exact seeks already in flight for other panes keep their
    // generations and the original global resume intent.
    seekBarrier_.detachPane(pane);
}

long long App::seekBarrierElapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - seekBarrierStarted_).count();
}

PaneArray<SeekBarrier::PaneStatus> App::paneStatuses() const {
    PaneArray<SeekBarrier::PaneStatus> status{};
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane]) continue;
        status[pane].loaded = true;
        status[pane].errored = !sources_[pane]->error().empty();
        const auto audioStatus = sources_[pane]->audioDecodeStatus();
        status[pane].audioGeneration = audioStatus.generation;
        status[pane].audioState = barrierAudioState(audioStatus.state);
    }
    return status;
}

bool App::seekBarrierReady() const {
    return seekBarrier_.ready(seekBarrierElapsedMs(), paneStatuses());
}

void App::completeSeekBarrier() {
    if (!seekBarrier_.active()) return;
    const bool resume = seekBarrier_.resume();
    const double target = seekBarrier_.target();
    const auto elapsed = seekBarrierElapsedMs();
    std::ostringstream message;
    message << "Seek barrier complete target=" << std::fixed << std::setprecision(3)
            << target << " elapsed_ms=" << elapsed
            << " timeout=" << seekBarrier_.timedOut() << " resume=" << resume;
    appendDiagnostic(message.str());
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane] || !seekBarrier_.audioWaiting(pane)) continue;
        const auto audioStatus = sources_[pane]->audioDecodeStatus();
        std::ostringstream audioMessage;
        audioMessage << "Seek audio slot=" << (pane + 1)
                     << " v=" << (positionForPane(pane) + 1)
                     << " generation=" << audioStatus.generation
                     << " state=" << audioDecodeStateName(audioStatus.state)
                     << " queued=" << audio_.hasQueuedAudio(pane);
        appendDiagnostic(audioMessage.str());
    }

    seekBarrier_.reset();
    clock_.seek(target);
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!sources_[pane]) continue;
        const bool useAudio = shouldOutputAudio(pane, target, resume);
        sources_[pane]->setAudioEnabled(useAudio);
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
    }
    if (resume) {
        audio_.play();
        clock_.play();
    } else {
        audio_.pause();
    }
    updateTitle();
    updateControls();
}

bool App::audioPaneEnabled(std::size_t pane) const {
    return pane < sources_.size() && paneSelected(audioMask_, pane);
}

bool App::shouldOutputAudio(std::size_t pane, double timelinePosition, bool playing) const {
    if (pane >= sources_.size()) return false;
    return shouldOutputPaneAudio(paneTiming(pane), audioPaneEnabled(pane),
                                 timelinePosition, playing, activeSeekMode());
}

bool App::playbackIntended() const {
    if (deviceRecoveryPending_) return deviceRecoveryResume_;
    return clock_.isPlaying() ||
           (seekBarrier_.active() && seekBarrier_.resume());
}

// The settings sheet reads the audio mask and per-pane trims when it is
// built, so there is nothing to push any more; kept for its call sites.
void App::refreshAudioControls() {}

void App::toggleAudioPane(std::size_t pane) {
    if (pane >= sources_.size()) return;
    setAudioMask(audioMask_ ^ (1U << pane));
    std::wstring selected;
    for (std::size_t position = 0; position < kMaxPanes; ++position) {
        if (!audioPaneEnabled(paneForPosition(position))) continue;
        if (!selected.empty()) selected += L" + ";
        selected += L"V" + std::to_wstring(position + 1);
    }
    showNotice(L"Audio: " + (selected.empty() ? L"none" : selected));
}

void App::setAudioMask(unsigned mask) {
    mask &= kAllPaneMask;
    const unsigned previous = audioMask_;
    audioMask_ = mask;
    const double position = clock_.position();
    const bool playing = clock_.isPlaying();
    const bool seekingTogether = seekBarrier_.active();
    if (previous != mask) {
        std::ostringstream message;
        message << "Audio mask old=0x" << std::hex << previous
                << " new=0x" << mask << std::dec
                << " timeline=" << std::fixed << std::setprecision(3) << position
                << " playing=" << playing
                << " barrier=" << seekingTogether
                << " barrier_resume="
                << (seekingTogether && seekBarrier_.resume());
        appendDiagnostic(message.str());
    }
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        const bool wasEnabled = (previous & (1U << pane)) != 0;
        const bool isEnabled = audioPaneEnabled(pane);
        if (wasEnabled == isEnabled) continue;
        if (isEnabled && embyPanes_[pane]) embyPanes_[pane]->addedMuted = false;
        if (isEnabled && localPanes_[pane]) localPanes_[pane]->addedMuted = false;
        if (!sources_[pane]) {
            audio_.flush(pane);
            continue;
        }
        if (!isEnabled) {
            sources_[pane]->setAudioEnabled(false);
            if (seekingTogether) seekBarrier_.setAudioWaiting(pane, false);
            const auto audioStatus = sources_[pane]->audioDecodeStatus();
            audio_.flush(pane);
            std::ostringstream message;
            message << "Audio output slot=" << (pane + 1)
                    << " v=" << (positionForPane(pane) + 1)
                    << " enabled=0 generation=" << audioStatus.generation;
            appendDiagnostic(message.str());
            continue;
        }
        if (seekingTogether && seekBarrier_.waiting(pane)) {
            sources_[pane]->setAudioEnabled(false);
            const auto audioGeneration = sources_[pane]->requestAudioSeek(
                seekBarrier_.paneTarget(pane));
            audio_.flush(pane, audioGeneration);
            seekBarrier_.setAudioWaiting(
                pane, seekBarrier_.resume() && seekBarrier_.waiting(pane));
            seekBarrier_.setAudioGeneration(pane, audioGeneration);
            sources_[pane]->setAudioEnabled(seekBarrier_.audioWaiting(pane));
            std::ostringstream message;
            message << "Audio output slot=" << (pane + 1)
                    << " v=" << (positionForPane(pane) + 1)
                    << " enabled=" << seekBarrier_.audioWaiting(pane)
                    << " mapped=" << std::fixed << std::setprecision(3)
                    << seekBarrier_.paneTarget(pane)
                    << " generation=" << audioGeneration
                    << " barrier=1";
            appendDiagnostic(message.str());
            continue;
        }
        const double mapped = mappedSourceTime(pane, position);
        sources_[pane]->setAudioEnabled(false);
        const auto audioGeneration = sources_[pane]->requestAudioSeek(mapped);
        audio_.flush(pane, audioGeneration);
        const bool useAudio = shouldOutputAudio(pane, position, playing);
        sources_[pane]->setAudioEnabled(useAudio);
        audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
        if (useAudio) audio_.play(pane);
        std::ostringstream message;
        message << "Audio output slot=" << (pane + 1)
                << " v=" << (positionForPane(pane) + 1)
                << " enabled=" << useAudio
                << " mapped=" << std::fixed << std::setprecision(3) << mapped
                << " generation=" << audioGeneration
                << " barrier=0";
        appendDiagnostic(message.str());
    }
    refreshAudioControls();
    updateHoverControls();
    updateTitle();
    scheduleAppSettingsSave();
}

void App::adjustVolumeFromWheel(short wheelDelta) {
    wheelDeltaRemainder_ += wheelDelta;
    const int steps = wheelDeltaRemainder_ / WHEEL_DELTA;
    wheelDeltaRemainder_ %= WHEEL_DELTA;
    if (steps == 0) return;
    const float volume = std::clamp(audio_.volume() + steps * 0.05F, 0.0F, 1.0F);
    audio_.setVolume(volume);
    if (volume > 0.0F && audio_.muted()) audio_.setMuted(false);
    const int slider = static_cast<int>(std::lround(volume * 100.0F));
    showNotice(L"Volume " + std::to_wstring(slider) + L"%");
    updateControls();
    updateTitle();
    scheduleAppSettingsSave();
}

void App::selectNextUnfinishedAudio(double timelinePosition) {
    // Automatic handoff follows the same visible V1-V5 order as the controls,
    // even when Auto layout's main pane is not storage slot zero.
    const auto order = panesByPosition();
    const auto timings = paneTimings();
    PaneTimings displayedTimings{};
    PaneSeekAudioStates displayedAudioStates{};
    unsigned displayedMask = 0;
    for (std::size_t position = 0; position < order.size(); ++position) {
        const auto pane = static_cast<std::size_t>(order[position]);
        displayedTimings[position] = timings[pane];
        if (((embyPanes_[pane] && embyPanes_[pane]->addedMuted) ||
             (localPanes_[pane] && localPanes_[pane]->addedMuted)) && !paneSelected(audioMask_, pane)) {
            // Add keeps a new pane silent until its audio is explicitly
            // selected. Existing sources retain automatic EOF handoff.
            displayedTimings[position].loaded = false;
        }
        if (paneSelected(audioMask_, pane)) displayedMask |= 1U << position;
        if (!sources_[pane]) continue;
        const auto status = sources_[pane]->audioDecodeStatus();
        auto state = barrierAudioState(status.state);
        // Ended/Error means the decoder cannot produce more data, not that the
        // SourceVoice has finished the buffers already accepted. Let those
        // buffers play before handing this selected bit to another pane.
        if ((state == SeekAudioState::Ended || state == SeekAudioState::Error) &&
            audio_.hasQueuedAudio(pane)) {
            state = SeekAudioState::Primed;
        }
        displayedAudioStates[position] = state;
    }
    const auto handoff = audioHandoff(
        displayedTimings, displayedMask, timelinePosition, activeSeekMode(),
        displayedAudioStates);
    if (!handoff.valid()) return;

    const auto current = static_cast<std::size_t>(order[static_cast<std::size_t>(handoff.from)]);
    const auto next = static_cast<std::size_t>(order[static_cast<std::size_t>(handoff.to)]);
    const auto finishedAudio = sources_[current]->audioDecodeStatus();
    audioMask_ = (audioMask_ & ~(1U << current)) | (1U << next);
    sources_[current]->setAudioEnabled(false);
    audio_.flush(current);
    sources_[next]->setAudioEnabled(false);
    const double mapped = mappedSourceTime(next, timelinePosition);
    const auto audioGeneration = sources_[next]->requestAudioSeek(mapped);
    audio_.flush(next, audioGeneration);
    {
        std::ostringstream message;
        message << "Audio handoff from_slot=" << (current + 1)
                << " from_v=" << (positionForPane(current) + 1)
                << " state=" << audioDecodeStateName(finishedAudio.state)
                << " to_slot=" << (next + 1)
                << " to_v=" << (positionForPane(next) + 1)
                << " generation=" << audioGeneration;
        appendDiagnostic(message.str());
    }
    const bool useAudio = shouldOutputAudio(next, timelinePosition, clock_.isPlaying());
    sources_[next]->setAudioEnabled(useAudio);
    audio_.setRate(next, static_cast<float>(playbackRates_[next]));
    if (useAudio) audio_.play(next);
    refreshAudioControls();
    updateTitle();
}

}  // namespace quaddeck
